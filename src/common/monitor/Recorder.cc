#include "common/monitor/Recorder.h"

#include <algorithm>
#include <folly/Likely.h>
#include <folly/logging/xlog.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/monitor/Monitor.h"
#include "common/monitor/Sample.h"

namespace hf3fs::monitor {

using namespace std::chrono_literals;
static std::string gHostname{0};
static std::string gPodname{0};

thread_local std::array<String, 65536> errorCodeStrings;
thread_local std::array<std::optional<TagSet>, 65536> errorCodeTagSets;

Recorder::~Recorder() {
  XLOGF_IF(DFATAL,
           register_,
           "Recorder forgot to unregister self with name {} and tags {}",
           name_,
           serde::toJsonString(tag_.asMap()));
}

void Recorder::collectAndClean(std::vector<Sample> &samples, bool cleanInactive) {
  collect(samples);

  auto iter = map_.begin();
  while (iter != map_.end()) {
    auto &recorder = iter->second;
    recorder->collect(samples);
    if (cleanInactive && !std::exchange(recorder->active_, false)) {
      XLOGF(DBG, "Remove inactive recorder {}", recorder->name());
      iter = map_.erase(iter);
    } else {
      ++iter;
    }
  }
}

void Recorder::setHostname(std::string hostname, std::string podname) {
  gHostname = std::move(hostname);
  gPodname = std::move(podname);
}

template <typename T>
Recorder::TagRef<T> Recorder::getRecorderWithTag(const TagSet &tag) {
  auto iter = map_.find(tag);
  if (UNLIKELY(iter == map_.end())) {
    TagSet new_tag_set = tag_;
    for (const auto &kv : tag) {
      new_tag_set.addTag(kv.first, kv.second);
    }
    auto newRecorder = std::make_unique<T>(*reinterpret_cast<T *>(this), new_tag_set);
    auto result = map_.insert(tag, std::move(newRecorder));
    iter = std::move(result.first);
  }
  return iter;
}

template <class ThreadLocalTag>
CountRecorderWithTLSTag<ThreadLocalTag>::CountRecorderWithTLSTag(std::string_view name,
                                                                 std::optional<TagSet> tag,
                                                                 bool resetWhenCollect)
    : CountRecorderWithTLSTag(Monitor::getDefaultInstance(), name, tag, resetWhenCollect) {}

template <class ThreadLocalTag>
void CountRecorderWithTLSTag<ThreadLocalTag>::addSample(int64_t val, const TagSet &tag) {
  getRecorderWithTag(tag)->addSample(val);
}

template <class ThreadLocalTag>
void CountRecorderWithTLSTag<ThreadLocalTag>::collect(std::vector<Sample> &samples) {
  // 1. collect for last second.
  int64_t sum = resetWhenCollect_ ? sum_.exchange(0) : sum_.load();
  for (auto &tls : tls_.accessAllThreads()) {
    sum += resetWhenCollect_ ? tls.exchange() : tls.load();
  }
  auto now = UtcClock::now();
  if (sum) {
    TagSet sample_tag = tag_;
    sample_tag.addTag("host", gHostname);
    sample_tag.addTag("pod", gPodname);
    samples.emplace_back(Sample{name_, std::move(sample_tag), now, sum});
    cumulativeVal_ += sum;
    active_ = true;
  }

  // 2. log per 30 seconds.
  if (resetWhenCollect_ && logPer30s_ && now - lastLogTime_ >= 30s) {
    if (cumulativeVal_ > 0) {
      XLOGF(DBG3, "Last 30s {}: {}", name_, cumulativeVal_);
      cumulativeVal_ = 0;
    }
    lastLogTime_ = now;
  }
}

template <class ThreadLocalTag>
void CountRecorderWithTLSTag<ThreadLocalTag>::collectAndClean(std::vector<Sample> &samples, bool cleanInactive) {
  Recorder::collectAndClean(samples, cleanInactive);
}

template <>
void CountRecorderWithTLSTag<AllocatedMemoryCounterTag>::collectAndClean(std::vector<Sample> &samples, bool) {
  Recorder::collectAndClean(samples, false /*cleanInactive*/);
}

template class CountRecorderWithTLSTag<SharedThreadLocalTag>;
template class CountRecorderWithTLSTag<AllocatedMemoryCounterTag>;

DistributionRecorder::DistributionRecorder(std::string_view name, std::optional<TagSet> tag)
    : DistributionRecorder(Monitor::getDefaultInstance(), name, tag) {}

void DistributionRecorder::collect(std::vector<Sample> &samples) {
  // 1. collect for last second.
  auto now = UtcClock::now();
  auto digest = tdigest_.build();
  if (!digest.empty()) {
    Distribution dist;
    dist.cnt = digest.count();
    dist.sum = digest.sum();
    dist.min = digest.min();
    dist.max = digest.max();
    dist.p50 = digest.estimateQuantile(0.50);
    dist.p90 = digest.estimateQuantile(0.90);
    dist.p95 = digest.estimateQuantile(0.95);
    dist.p99 = digest.estimateQuantile(0.99);
    samples.emplace_back();
    samples.back().name = name_;

    TagSet sample_tag = tag_;
    sample_tag.addTag("host", gHostname);
    sample_tag.addTag("pod", gPodname);
    samples.back().tags = std::move(sample_tag);

    samples.back().timestamp = now;
    samples.back().value = dist;
    active_ = true;

    if (logPer30s_) {
      cumulativeDigests_.push_back(std::move(digest));
    }
  }

  // 2. log per 30 seconds.
  if (logPer30s_ && now - lastLogTime_ >= 30s) {
    if (!cumulativeDigests_.empty()) {
      logPer30s(folly::TDigest::merge(folly::Range(&cumulativeDigests_[0], cumulativeDigests_.size())));
      cumulativeDigests_.clear();
    }
    lastLogTime_ = now;
  }
}

void DistributionRecorder::addSample(double val, const TagSet &tag) { getRecorderWithTag(tag)->addSample(val); }

void DistributionRecorder::logPer30s(const folly::TDigest &digest) {
  XLOGF(DBG3,
        "Last 30s {}: count:{} mean:{:.1f} min:{:.1f} max:{:.1f} p50:{:.1f} p90:{:.1f} p95:{:.1f} p99:{:.1f}",
        name_,
        uint64_t(digest.count()),
        digest.mean(),
        digest.min(),
        digest.max(),
        digest.estimateQuantile(0.50),
        digest.estimateQuantile(0.90),
        digest.estimateQuantile(0.95),
        digest.estimateQuantile(0.99));
}

LatencyRecorder::LatencyRecorder(std::string_view name, std::optional<TagSet> tag)
    : DistributionRecorder(Monitor::getDefaultInstance(), name, tag, false) {
  registerRecorder();
}

LatencyRecorder::LatencyRecorder(LatencyRecorder &parent, const TagSet &tag)
    : DistributionRecorder(parent.monitor_, parent.name(), tag, false) {}

SimpleDistributionRecorder::SimpleDistributionRecorder(std::string_view name, std::optional<TagSet> tag)
    : SimpleDistributionRecorder(Monitor::getDefaultInstance(), name, tag) {}

void SimpleDistributionRecorder::collect(std::vector<Sample> &samples) {
  int64_t sum = sum_.exchange(0, std::memory_order_relaxed);
  int64_t count = count_.exchange(0, std::memory_order_relaxed);
  int64_t minval = min_.exchange(std::numeric_limits<int64_t>::max(), std::memory_order_relaxed);
  int64_t maxval = max_.exchange(0, std::memory_order_relaxed);

  for (auto &tls : tls_.accessAllThreads()) {
    sum += tls.exchangeSum();
    count += tls.exchangeCount();
    minval = std::min(minval, tls.exchangeMin());
    maxval = std::max(maxval, tls.exchangeMax());
  }

  auto now = UtcClock::now();
  if (count != 0 && sum != 0) {
    Distribution dist;
    dist.cnt = count;
    dist.sum = sum;
    if (minval != std::numeric_limits<int64_t>::max()) {
      dist.min = minval;
    }
    dist.max = maxval;
    samples.emplace_back();
    samples.back().name = name_;

    TagSet sample_tag = tag_;
    sample_tag.addTag("host", gHostname);
    sample_tag.addTag("pod", gPodname);
    samples.back().tags = std::move(sample_tag);

    samples.back().timestamp = now;
    samples.back().value = dist;
    active_ = true;
  }
}

void SimpleDistributionRecorder::addSample(int64_t val, const TagSet &tag) { getRecorderWithTag(tag)->addSample(val); }

void LatencyRecorder::addSample(std::chrono::nanoseconds duration, const TagSet &tag) {
  getRecorderWithTag(tag)->addSample(duration);
}

void LatencyRecorder::logPer30s(const folly::TDigest &digest) {
  XLOGF(DBG3,
        "Last 30s {}: count:{} mean:{}us min:{}us max:{}us p50:{}us p90:{}us p95:{}us p99:{}us",
        name_,
        uint64_t(digest.count()),
        uint64_t(digest.mean()) / 1000,
        uint64_t(digest.min()) / 1000,
        uint64_t(digest.max()) / 1000,
        uint64_t(digest.estimateQuantile(0.50)) / 1000,
        uint64_t(digest.estimateQuantile(0.90)) / 1000,
        uint64_t(digest.estimateQuantile(0.95)) / 1000,
        uint64_t(digest.estimateQuantile(0.99)) / 1000);
}

template <typename LatencyRecorderT>
OperationRecorderT<LatencyRecorderT>::OperationRecorderT(std::string_view name,
                                                         std::optional<TagSet> tag,
                                                         bool recordErrorCode)
    : total_(fmt::format("{}.total", name), tag),
      fails_(fmt::format("{}.fails", name), tag),
      current_(fmt::format("{}.current", name), tag, /*resetWhenCollect=*/false),
      succ_latencies_(fmt::format("{}.succ_latency", name), tag),
      fail_latencies_(fmt::format("{}.fail_latency", name), tag),
      recordErrorCode_(recordErrorCode) {}

template <typename LatencyRecorderT>
void OperationRecorderT<LatencyRecorderT>::Guard::reportWithCode(status_code_t code) {
  if (reported_) {
    return;
  }
  success_ |= (code == StatusCode::kOK);
  auto latency = RelativeTime::now() - startTime_;

  if (tags_.has_value()) {
    recorder_.current_.addSample(-1, *tags_);
    if (LIKELY(success_)) {
      recorder_.succ_latencies_.addSample(latency, *tags_);
    } else {
      if (recorder_.recordErrorCode_) {
        auto &errorCodeStr = errorCodeStrings[code];
        if (errorCodeStr.empty()) {
          errorCodeStr = StatusCode::toString(code);
        }
        recorder_.fails_.addSample(1, tags_->newTagSet("statusCode", errorCodeStr));
      } else {
        recorder_.fails_.addSample(1, *tags_);
      }
      recorder_.fail_latencies_.addSample(latency, *tags_);
    }
  } else {
    recorder_.current_.addSample(-1);
    if (LIKELY(success_)) {
      recorder_.succ_latencies_.addSample(latency);
    } else {
      if (recorder_.recordErrorCode_) {
        auto &tagset = errorCodeTagSets[code];
        if (!tagset) {
          tagset = TagSet::create("statusCode", String(StatusCode::toString(code)));
        }
        recorder_.fails_.addSample(1, *tagset);
      } else {
        recorder_.fails_.addSample(1);
      }
      recorder_.fail_latencies_.addSample(latency);
    }
  }
  reported_ = true;
}

template class OperationRecorderT<LatencyRecorder>;
template class OperationRecorderT<SimpleDistributionRecorder>;

ValueRecorder::ValueRecorder(std::string_view name, std::optional<TagSet> tag, bool resetWhenCollect)
    : ValueRecorder(Monitor::getDefaultInstance(), name, tag, resetWhenCollect) {}

void ValueRecorder::set(int64_t val, const TagSet &tag) { getRecorderWithTag(tag)->set(val); }

void ValueRecorder::collect(std::vector<Sample> &samples) {
  int64_t val = resetWhenCollect_ ? val_.exchange(0) : val_.load();
  if (val > 0) {
    auto now = UtcClock::now();
    TagSet sample_tag = tag_;
    sample_tag.addTag("host", gHostname);
    sample_tag.addTag("pod", gPodname);
    samples.emplace_back(Sample{name_, std::move(sample_tag), now, val});
    active_ = true;
  }
}

void ValueRecorder::collectAndClean(std::vector<Sample> &samples, bool cleanInactive) {
  Recorder::collectAndClean(samples, cleanInactive);
}

LambdaRecorder::LambdaRecorder(std::string_view name, std::optional<TagSet> tag /* = std::nullopt */)
    : Recorder(name, tag, Monitor::getDefaultInstance()) {
  registerRecorder();
}

void LambdaRecorder::collect(std::vector<Sample> &samples) {
  auto lock = std::unique_lock(mutex_);
  auto value = getter_();
  lock.unlock();
  if (value) {
    TagSet sampleTag = tag_;
    sampleTag.addTag("host", gHostname);
    sampleTag.addTag("pod", gPodname);
    samples.emplace_back(Sample{name_, std::move(sampleTag), UtcClock::now(), value});
  }
}

}  // namespace hf3fs::monitor
