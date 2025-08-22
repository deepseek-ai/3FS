#pragma once
#include <algorithm>
#include <climits>
#include <cstddef>

#include "common/monitor/Recorder.h"

namespace hf3fs::memory {

class AllocatedMemoryCounter {
 public:
  AllocatedMemoryCounter();

  ~AllocatedMemoryCounter();

  static void initialize();

  static void shutdown();

  void add(size_t size) {
    allocatedMemory_[0] += size;
    allocatedMemory_[calcBucket(size)] += size;
    tryReport();
  }

  void sub(size_t size) {
    deallocatedMemory_[0] += size;
    deallocatedMemory_[calcBucket(size)] += size;
    tryReport();
  }

 private:
  static size_t calcBucket(size_t size) {
    size_t sizeWidth = SIZE_WIDTH - __builtin_clzll(size);
    size_t bucket = sizeWidth > kMinBucketSizeWidth ? sizeWidth - kMinBucketSizeWidth + 1 : 1;
    return std::min(bucket, kMaxNumBuckets - 1);
  }

  static size_t calcBucketSize(size_t bucketIndex) { return 1ULL << (kMinBucketSizeWidth + bucketIndex - 2); }

  static void initFromEnvVars();

  static void initBucketTagSets();

  void initThreadTagSet();

  void tryReport(bool force = false);

 public:
  static constexpr size_t kMinBucketSizeWidth = 9;
  static constexpr size_t kMinBucketSizeBytes = 1 << (kMinBucketSizeWidth - 1);  // 256 bytes
  static constexpr size_t kMaxNumBuckets = 20;

 private:
  monitor::TagSet threadTagSet_;
  bool initialized_;
  bool reporting_;

  ssize_t allocatedMemory_[kMaxNumBuckets] = {0};
  ssize_t deallocatedMemory_[kMaxNumBuckets] = {0};
};

}  // namespace hf3fs::memory
