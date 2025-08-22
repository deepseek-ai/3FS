#pragma once

#include <algorithm>
#include <chrono>

#include "common/utils/UtcTime.h"

namespace hf3fs {

class ExponentialBackoffRetry {
 public:
  ExponentialBackoffRetry(std::chrono::milliseconds initWaitTime,
                          std::chrono::milliseconds maxWaitTime,
                          std::chrono::milliseconds maxRetryTime)
      : startTime_(SteadyClock::now()),
        initWaitTime_(initWaitTime),
        maxWaitTime_(std::max(initWaitTime_, maxWaitTime)),
        maxRetryTime_(std::max(maxWaitTime_, maxRetryTime)),
        nextWaitTime_(initWaitTime) {}

  std::chrono::milliseconds getWaitTime() {
    auto elapsedTime = getElapsedTime();

    if (elapsedTime + initWaitTime_ > maxRetryTime_) {
      return std::chrono::milliseconds::zero();
    }

    std::chrono::milliseconds waitTime = std::min(nextWaitTime_, maxRetryTime_ - elapsedTime);
    nextWaitTime_ = std::min(nextWaitTime_ * kWaitTimeMultiplier, maxWaitTime_);

    return waitTime;
  }

  hf3fs::SteadyTime getStartTime() const { return startTime_; }

  std::chrono::milliseconds getElapsedTime() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - startTime_);
  }

 private:
  static constexpr uint32_t kWaitTimeMultiplier = 2;

  const hf3fs::SteadyTime startTime_;
  const std::chrono::milliseconds initWaitTime_;
  const std::chrono::milliseconds maxWaitTime_;
  const std::chrono::milliseconds maxRetryTime_;
  std::chrono::milliseconds nextWaitTime_;
};
}  // namespace hf3fs
