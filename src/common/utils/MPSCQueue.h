#pragma once

#include <atomic>
#include <folly/Likely.h>
#include <memory>
#include <vector>

#include "common/utils/Status.h"

namespace hf3fs {

template <typename T>
class MPSCQueue {
 public:
  explicit MPSCQueue(uint64_t capacity)
      : capacity_(capacity),
        items_(std::make_unique<std::unique_ptr<T>[]>(capacity)) {}

  uint64_t size() const {
    auto t = tail_.load(std::memory_order_acquire);
    auto h = head_.load(std::memory_order_acquire);
    return h - t;
  }
  bool empty() const { return size() == 0; }
  bool full() const { return size() >= capacity_; }

  Status pop(std::unique_ptr<T> *item) {
    auto t = tail_.load(std::memory_order_acquire);
    auto h = head_.load(std::memory_order_acquire);

    if (h == t) {
      return Status(StatusCode::kQueueEmpty);
    }

    auto &pop = items_[t % capacity_];
    if (UNLIKELY(pop == nullptr)) {
      return Status(StatusCode::kQueueConflict);
    }
    *item = std::move(pop);

    ++tail_;
    return Status::OK;
  }

  Status push(std::unique_ptr<T> *item, int retry_times = 5) {
    if (UNLIKELY((item->get() == nullptr))) {
      return Status(StatusCode::kQueueInvalidItem);
    }

    for (int i = 0; i < retry_times || retry_times == -1; ++i) {
      auto t = tail_.load(std::memory_order_acquire);
      auto h = head_.load(std::memory_order_acquire);

      if (UNLIKELY(h - t >= capacity_)) {
        return Status(StatusCode::kQueueFull);
      }

      // CAS
      if (head_.compare_exchange_strong(h, h + 1)) {
        item->swap(items_[h % capacity_]);
        return Status::OK;
      }
    }

    return Status(StatusCode::kQueueConflict);
  }

 private:
  const uint64_t capacity_;
  std::unique_ptr<std::unique_ptr<T>[]> items_;

  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) std::atomic<uint64_t> tail_{0};
};
static_assert(sizeof(MPSCQueue<int>) == 3 * 64, "sizeof(MPSCQueue) != 3* 64");

}  // namespace hf3fs
