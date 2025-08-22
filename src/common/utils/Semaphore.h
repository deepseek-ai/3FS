#pragma once

#include <folly/experimental/symbolizer/Symbolizer.h>
#include <folly/fibers/Semaphore.h>
#include <folly/logging/xlog.h>

namespace hf3fs {

/* Semaphore with changeable effective capacity and fixed max capacity */
class Semaphore : public folly::NonCopyableNonMovable {
 public:
  Semaphore(size_t usableTokens, size_t maxTokens = 4096)
      : semaphore_(maxTokens),
        maxTokens_(maxTokens),
        usableTokens_(std::min(usableTokens, maxTokens)) {
    XLOGF_IF(CRITICAL,
             usableTokens > maxTokens,
             "usableTokens {} > maxTokens {}, stack trace:\n{}",
             usableTokens,
             maxTokens,
             folly::symbolizer::getStackTraceStr());
    for (size_t i = usableTokens_; i < maxTokens_; i++) semaphore_.wait();
    XLOGF(INFO,
          "usableTokens: {}, maxTokens: {}, availableTokens: {}",
          usableTokens_,
          maxTokens_,
          semaphore_.getAvailableTokens());
  }

  size_t getMaxTokens() const { return maxTokens_; }

  size_t getUsableTokens() const {
    std::scoped_lock<std::mutex> lock(usableTokensMutex_);
    return usableTokens_;
  }

  void signal() { semaphore_.signal(); }

  void wait() { semaphore_.wait(); }

  bool try_wait() { return semaphore_.try_wait(); }

  folly::coro::Task<void> co_wait() { return semaphore_.co_wait(); }

  void changeUsableTokens(size_t newUsableTokens) {
    std::scoped_lock<std::mutex> lock(usableTokensMutex_);
    XLOGF(WARN, "Try to update usableTokens from {} to {}", usableTokens_, newUsableTokens);
    newUsableTokens = std::min(newUsableTokens, maxTokens_);
    for (size_t i = newUsableTokens; i < usableTokens_; i++) semaphore_.wait();
    for (size_t i = usableTokens_; i < newUsableTokens; i++) semaphore_.signal();
    XLOGF(WARN, "Updated usableTokens from {} to {}", usableTokens_, newUsableTokens);
    usableTokens_ = newUsableTokens;
  }

 private:
  folly::fibers::Semaphore semaphore_;
  const size_t maxTokens_;
  size_t usableTokens_;
  mutable std::mutex usableTokensMutex_;
};
}  // namespace hf3fs
