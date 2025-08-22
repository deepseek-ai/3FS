#include "AllocatedMemoryCounter.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <folly/system/ThreadName.h>

#include "common/monitor/Recorder.h"

namespace hf3fs::memory {

using AllocatedMemoryCountRecorder = monitor::CountRecorderWithTLSTag<monitor::AllocatedMemoryCounterTag>;

static AllocatedMemoryCountRecorder *used_bytes[AllocatedMemoryCounter::kMaxNumBuckets];
static AllocatedMemoryCountRecorder *allocated_bytes[AllocatedMemoryCounter::kMaxNumBuckets];
static AllocatedMemoryCountRecorder *deallocated_bytes[AllocatedMemoryCounter::kMaxNumBuckets];
static AllocatedMemoryCountRecorder *thread_allocated_bytes[AllocatedMemoryCounter::kMaxNumBuckets];
static AllocatedMemoryCountRecorder *thread_deallocated_bytes[AllocatedMemoryCounter::kMaxNumBuckets];
static AllocatedMemoryCountRecorder *thread_accum_allocated_bytes[AllocatedMemoryCounter::kMaxNumBuckets];
static AllocatedMemoryCountRecorder *thread_accum_deallocated_bytes[AllocatedMemoryCounter::kMaxNumBuckets];

static std::atomic<bool> gInitializing = false;
static std::atomic<bool> gInitialized = false;
static std::atomic<bool> gShutdown = false;
static ssize_t gMemoryMetricReportInterval = 100_MB;
static bool gThreadCounterEnabled = false;

void AllocatedMemoryCounter::initFromEnvVars() {
  // If environment variable is not defined, return the default value.
  const char *reportIntervalStr = std::getenv("MEMORY_METRIC_REPORT_INTERVAL");

  if (reportIntervalStr != nullptr && reportIntervalStr[0] != '\0') {
    ssize_t reportInterval = 0;
    int result = sscanf(reportIntervalStr, "%zd", &reportInterval);
    if (result != 1 || reportInterval <= 0) {
      fprintf(stderr, "Invalid value: MEMORY_METRIC_REPORT_INTERVAL=\"%s\"\n", reportIntervalStr);
    } else {
      gMemoryMetricReportInterval = reportInterval;
    }
  }

#ifndef NDEBUG
  fprintf(stderr, "Set memory metric report interval to every %zd bytes\n", gMemoryMetricReportInterval);
#endif

  const char *threadCounterEnabledStr = std::getenv("MEMORY_METRIC_THREAD_COUNTER");

  if (threadCounterEnabledStr != nullptr && threadCounterEnabledStr[0] != '\0') {
    gThreadCounterEnabled = true;
  }

#ifndef NDEBUG
  fprintf(stderr, "Enable per-thread memory counter: %s\n", gThreadCounterEnabled ? "yes" : "no");
#endif
}

void AllocatedMemoryCounter::initBucketTagSets() {
  for (size_t bucketIndex = 0; bucketIndex < AllocatedMemoryCounter::kMaxNumBuckets; bucketIndex++) {
    std::string bucketTag =
        bucketIndex == 0 ? "Total" : fmt::format("{}B", AllocatedMemoryCounter::calcBucketSize(bucketIndex));
    auto bucketTagSet = monitor::instanceTagSet(bucketTag);
    used_bytes[bucketIndex] =
        new AllocatedMemoryCountRecorder("memory_allocator.used_bytes", bucketTagSet, false /*reset*/);
    allocated_bytes[bucketIndex] =
        new AllocatedMemoryCountRecorder("memory_allocator.allocated_bytes", bucketTagSet, true /*reset*/);
    deallocated_bytes[bucketIndex] =
        new AllocatedMemoryCountRecorder("memory_allocator.deallocated_bytes", bucketTagSet, true /*reset*/);
    thread_allocated_bytes[bucketIndex] =
        new AllocatedMemoryCountRecorder("memory_allocator.thread_allocated_bytes", bucketTagSet, true /*reset*/);
    thread_deallocated_bytes[bucketIndex] =
        new AllocatedMemoryCountRecorder("memory_allocator.thread_deallocated_bytes", bucketTagSet, true /*reset*/);
    thread_accum_allocated_bytes[bucketIndex] =
        new AllocatedMemoryCountRecorder("memory_allocator.thread_accum_allocated_bytes",
                                         bucketTagSet,
                                         false /*reset*/);
    thread_accum_deallocated_bytes[bucketIndex] =
        new AllocatedMemoryCountRecorder("memory_allocator.thread_accum_deallocated_bytes",
                                         bucketTagSet,
                                         false /*reset*/);
  }

#ifndef NDEBUG
  fprintf(stderr, "Allocated memory counter initialized (%zu buckets)\n", AllocatedMemoryCounter::kMaxNumBuckets);
#endif
}

void AllocatedMemoryCounter::initThreadTagSet() {
  std::string_view threadName = "(null)";
  auto currentThreadName = folly::getCurrentThreadName();
  if (currentThreadName) threadName = *currentThreadName;
  threadTagSet_ = monitor::threadTagSet(threadName);
}

void AllocatedMemoryCounter::initialize() {
  if (gShutdown || gInitialized) return;

  bool initializing = gInitializing.exchange(true);

  if (!initializing) {
    initFromEnvVars();
    initBucketTagSets();
    gInitialized = true;
  }
}

void AllocatedMemoryCounter::shutdown() { gShutdown = true; }

AllocatedMemoryCounter::AllocatedMemoryCounter()
    : initialized_(false),
      reporting_(false) {
  initialize();
  initThreadTagSet();
  initialized_ = true;
}

AllocatedMemoryCounter::~AllocatedMemoryCounter() { tryReport(true /*force*/); }

void AllocatedMemoryCounter::tryReport(bool force) {
  // disable reporting during shutdown and initialization to avoid using the destroyed or uninitialized tag sets
  if (reporting_ || !initialized_ || gShutdown) return;

  reporting_ = true;

  ssize_t totalAllocated = allocatedMemory_[0];
  ssize_t totalDeallocated = deallocatedMemory_[0];
  bool reportMemUsage = totalAllocated + totalDeallocated >= gMemoryMetricReportInterval;

  if (force || reportMemUsage) {
    for (size_t bucketIndex = 0; bucketIndex < kMaxNumBuckets; bucketIndex++) {
      ssize_t allocated = allocatedMemory_[bucketIndex];
      ssize_t deallocated = deallocatedMemory_[bucketIndex];
      ssize_t changed = allocated - deallocated;
      // first clear the (de)allocated memory
      allocatedMemory_[bucketIndex] = 0;
      deallocatedMemory_[bucketIndex] = 0;
      // then report metrics
      if (changed) {
        used_bytes[bucketIndex]->addSample(changed);
      }
      if (allocated) {
        allocated_bytes[bucketIndex]->addSample(allocated);
        if (gThreadCounterEnabled && reportMemUsage) {
          thread_allocated_bytes[bucketIndex]->addSample(allocated, threadTagSet_);
          thread_accum_allocated_bytes[bucketIndex]->addSample(allocated, threadTagSet_);
        }
      }
      if (deallocated) {
        deallocated_bytes[bucketIndex]->addSample(deallocated);
        if (gThreadCounterEnabled && reportMemUsage) {
          thread_deallocated_bytes[bucketIndex]->addSample(deallocated, threadTagSet_);
          thread_accum_deallocated_bytes[bucketIndex]->addSample(deallocated, threadTagSet_);
        }
      }
    }
  }

  reporting_ = false;
}

}  // namespace hf3fs::memory
