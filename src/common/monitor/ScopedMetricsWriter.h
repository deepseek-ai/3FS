#pragma once

#include "Recorder.h"

namespace hf3fs::monitor {

class ScopedCounterWriter {
 public:
  ScopedCounterWriter(monitor::DistributionRecorder &recorder,
                      std::atomic_int64_t &counter,
                      int64_t value,
                      const monitor::TagSet &tagSet)
      : recorder_(recorder),
        counter_(counter),
        value_(value),
        tagSet_(tagSet) {
    int64_t currentCounter = counter_.fetch_add(value_) + value_;
    if (tagSet.size() > 0)
      recorder_.addSample(currentCounter, tagSet_);
    else
      recorder_.addSample(currentCounter);
  }

  ScopedCounterWriter(monitor::DistributionRecorder &recorder,
                      std::atomic_int64_t &counter,
                      int64_t value,
                      const std::string &instance = "")
      : ScopedCounterWriter(recorder,
                            counter,
                            value,
                            instance.empty() ? monitor::TagSet() : monitor::instanceTagSet(instance)) {}

  ~ScopedCounterWriter() {
    int64_t currentCounter = counter_.fetch_add(-value_) - value_;
    if (tagSet_.size() > 0)
      recorder_.addSample(currentCounter, tagSet_);
    else
      recorder_.addSample(currentCounter);
  }

 private:
  monitor::DistributionRecorder &recorder_;
  std::atomic_int64_t &counter_;
  int64_t value_;
  const monitor::TagSet tagSet_;
};

class ScopedLatencyWriter {
 public:
  ScopedLatencyWriter(monitor::LatencyRecorder &recorder, const monitor::TagSet &tagSet)
      : recorder_(recorder),
        startTime_(hf3fs::SteadyClock::now()),
        tagSet_(tagSet) {}

  ScopedLatencyWriter(monitor::LatencyRecorder &recorder, const std::string &instance = "")
      : ScopedLatencyWriter(recorder, instance.empty() ? monitor::TagSet() : monitor::instanceTagSet(instance)) {}

  ~ScopedLatencyWriter() {
    if (tagSet_.size() > 0)
      recorder_.addSample(getElapsedTime(), tagSet_);
    else
      recorder_.addSample(getElapsedTime());
  }

  hf3fs::SteadyClock::duration getElapsedTime() const { return hf3fs::SteadyClock::now() - startTime_; }

 private:
  monitor::LatencyRecorder &recorder_;
  hf3fs::SteadyClock::time_point startTime_;
  const monitor::TagSet tagSet_;
};

}  // namespace hf3fs::monitor
