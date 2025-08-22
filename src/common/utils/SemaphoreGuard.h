#pragma once

#include <folly/logging/xlog.h>

#include "Semaphore.h"

namespace hf3fs {
class SemaphoreGuard : public folly::NonCopyableNonMovable {
 public:
  SemaphoreGuard(Semaphore &semaphore)
      : semaphore_(semaphore),
        acquired_(false) {}

  ~SemaphoreGuard() {
    if (acquired_) semaphore_.signal();
  }

  void wait() {
    semaphore_.wait();
    acquired_ = true;
  }

  bool tryWait() {
    acquired_ = semaphore_.try_wait();
    return acquired_;
  }

  folly::coro::Task<void> coWait() {
    acquired_ = semaphore_.try_wait();
    if (acquired_) co_return;

    XLOGF(DBG5, "Cannot acquire semaphore {} right now, waiting", fmt::ptr(&semaphore_));
    co_await semaphore_.co_wait();
    acquired_ = true;
  }

 private:
  Semaphore &semaphore_;
  bool acquired_;
};
}  // namespace hf3fs
