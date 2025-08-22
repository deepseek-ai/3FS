#pragma once

#include <folly/MPMCQueue.h>
#include <folly/ProducerConsumerQueue.h>
#include <folly/Utility.h>
#include <folly/concurrency/PriorityUnboundedQueueSet.h>
#include <folly/experimental/coro/Task.h>
#include <folly/fibers/Semaphore.h>
#include <optional>

#include "common/utils/Semaphore.h"

namespace hf3fs {

/** Like folly::PriorityUnboundedBlockingQueue, but work with coroutine. */
template <typename T>
class PriorityUnboundedQueue {
 public:
  explicit PriorityUnboundedQueue(uint8_t numPriorities)
      : sem_(0),
        queue_(numPriorities) {}

  uint8_t getNumPriorities() { return queue_.priorities(); }

  void addWithPriority(T item, int8_t priority) {
    queue_.at_priority(translatePriority(priority)).enqueue(std::move(item));
    sem_.signal();
  }

  T dequeue() {
    sem_.wait();
    return dequeueImpl();
  }

  std::optional<T> try_dequeue() {
    if (!sem_.try_wait()) {
      return std::nullopt;
    }
    return dequeueImpl();
  }

  folly::coro::Task<T> co_dequeue() {
    co_await sem_.co_wait();
    co_return dequeueImpl();
  }

  size_t size() const { return queue_.size(); }

  size_t sizeGuess() const { return queue_.size(); }

 private:
  size_t translatePriority(int8_t const priority) {
    size_t const priorities = queue_.priorities();
    assert(priorities <= 255);
    int8_t const hi = (priorities + 1) / 2 - 1;
    int8_t const lo = hi - (priorities - 1);
    return hi - folly::constexpr_clamp(priority, lo, hi);
  }

  T dequeueImpl() {
    // must follow a successful sem wait
    if (auto obj = queue_.try_dequeue()) {
      return std::move(*obj);
    }
    folly::terminate_with<std::logic_error>("bug in PriorityUnboundedQueue");
  }

  folly::fibers::Semaphore sem_;
  folly::PriorityUnboundedQueueSet<T, false, false, true /* MayBlock */> queue_;
};

}  // namespace hf3fs