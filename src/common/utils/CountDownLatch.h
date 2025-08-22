#pragma once

#include <atomic>
#include <folly/experimental/coro/Baton.h>

#include "common/utils/Coroutine.h"

namespace hf3fs {
template <typename BatonType = folly::coro::Baton>
class CountDownLatch {
 public:
  explicit CountDownLatch(int64_t count = 0)
      : count_(count) {}

  void countDown(int64_t n = 1) {
    auto remaining = count_.fetch_sub(n, std::memory_order_acq_rel);
    assert(remaining >= n);
    if (remaining == n) {
      baton_.post();
    }
  }

  int64_t increase(int64_t n = 1) { return count_.fetch_add(n, std::memory_order_acq_rel); }

  CoTask<void> wait() {
    if (count_.load(std::memory_order_acquire) > 0) co_await baton_;
  }

  bool tryWait() { return count_.load(std::memory_order_acquire) == 0; }

  void reset() { baton_.reset(); }

 private:
  std::atomic<int64_t> count_;
  BatonType baton_;
};
}  // namespace hf3fs
