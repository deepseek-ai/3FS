#pragma once

#include <folly/MPMCQueue.h>
#include <folly/ProducerConsumerQueue.h>
#include <folly/Utility.h>
#include <folly/experimental/coro/Task.h>
#include <folly/fibers/Semaphore.h>

namespace hf3fs {

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(uint32_t capacity)
      : queue_(capacity),
        enqueueSemaphore_{capacity},
        dequeueSemaphore_{0} {}
  BoundedQueue(const BoundedQueue &) = delete;
  BoundedQueue &operator=(const BoundedQueue &) = delete;

  template <typename U = T>
  void enqueue(U &&item) {
    enqueueSemaphore_.wait();
    queue_.writeIfNotFull(std::forward<U>(item));
    dequeueSemaphore_.signal();
  }

  template <typename U = T>
  folly::coro::Task<void> co_enqueue(U &&item) {
    co_await enqueueSemaphore_.co_wait();
    queue_.writeIfNotFull(std::forward<U>(item));
    dequeueSemaphore_.signal();
  }

  T dequeue() {
    dequeueSemaphore_.wait();
    T item;
    queue_.readIfNotEmpty(item);
    enqueueSemaphore_.signal();
    return item;
  }

  folly::coro::Task<T> co_dequeue() {
    co_await dequeueSemaphore_.co_wait();
    T item;
    queue_.readIfNotEmpty(item);
    enqueueSemaphore_.signal();
    co_return item;
  }

  void dequeue(T &item) {
    dequeueSemaphore_.wait();
    queue_.readIfNotEmpty(item);
    enqueueSemaphore_.signal();
  }

  folly::coro::Task<void> co_dequeue(T &item) {
    co_await dequeueSemaphore_.co_wait();
    queue_.readIfNotEmpty(item);
    enqueueSemaphore_.signal();
  }

  template <typename U = T>
  bool try_enqueue(U &&item) {
    auto waitSuccess = enqueueSemaphore_.try_wait();
    if (!waitSuccess) {
      return false;
    }
    queue_.writeIfNotFull(std::forward<U>(item));
    dequeueSemaphore_.signal();
    return true;
  }

  std::optional<T> try_dequeue() {
    T item;
    if (try_dequeue(item)) {
      return item;
    }
    return std::nullopt;
  }

  bool try_dequeue(T &item) {
    auto waitSuccess = dequeueSemaphore_.try_wait();
    if (!waitSuccess) {
      return false;
    }
    queue_.readIfNotEmpty(item);
    enqueueSemaphore_.signal();
    return true;
  }

  bool empty() const { return queue_.isEmpty(); }
  bool full() const { return queue_.isFull(); }
  size_t size() const { return queue_.size(); }

 private:
  folly::MPMCQueue<T> queue_;
  folly::fibers::Semaphore enqueueSemaphore_;
  folly::fibers::Semaphore dequeueSemaphore_;
};

}  // namespace hf3fs
