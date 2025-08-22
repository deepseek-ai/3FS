#pragma once

#include <folly/Random.h>
#include <folly/Synchronized.h>
#include <folly/executors/task_queue/UnboundedBlockingQueue.h>

#include "common/utils/SimpleSemaphore.h"

namespace hf3fs {
namespace details {
template <typename EndPredicate, typename T>
class EndValueHolder {
 public:
  explicit EndValueHolder(EndPredicate predicate)
      : isEndPredicate_(std::move(predicate)) {}

  bool add(T &&item) {
    if (UNLIKELY(isEndPredicate_(item))) {
      {
        auto guard = endValue_.lock();
        *guard = std::move(item);
      }
      isEnd_.store(true, std::memory_order_release);
      return true;
    }
    return false;
  }

  bool hasValue() const { return isEnd_.load(std::memory_order_acquire); }

  T forceTake() {
    auto guard = endValue_.lock();
    return std::move(**guard);
  }

  folly::Optional<T> take() {
    if (UNLIKELY(isEnd_.load(std::memory_order_acquire))) {
      auto guard = endValue_.lock();
      return std::move(**guard);
    }
    return {};
  }

 private:
  EndPredicate isEndPredicate_;
  std::atomic<bool> isEnd_{false};
  folly::Synchronized<std::optional<T>, std::mutex> endValue_;
};

template <typename T>
class SharedNothingQueueStrategy {
 public:
  using ItemType = T;
  using Queue = folly::UnboundedBlockingQueue<T, SimpleSemaphore>;
  using QueuePtr = std::shared_ptr<Queue>;

  struct SharedState {
    explicit SharedState(size_t count) {
      queues.reserve(count);
      for (uint32_t i = 0; i < count; ++i) queues.push_back(std::make_shared<Queue>());
    }

    std::vector<QueuePtr> queues;
  };

  SharedNothingQueueStrategy(const std::shared_ptr<SharedState> &state, uint32_t selfPos)
      : self_(state->queues[selfPos]) {}

  void add(T &&item) { self_->add(std::move(item)); }

  size_t queueSize() const { return self_->size(); }

  template <typename EndValueHolder>
  folly::Optional<T> tryTake(std::chrono::milliseconds time, EndValueHolder &endValue) {
    if (auto item = self_->try_take_for(time); item) {
      return item;
    }
    return endValue.take();
  }

 private:
  QueuePtr self_;
};

template <typename T>
class WorkStealingQueueStrategy {
 public:
  using ItemType = T;
  using Queue = folly::UnboundedBlockingQueue<T, SimpleSemaphore>;
  using QueuePtr = std::shared_ptr<Queue>;

  struct SharedState {
    explicit SharedState(size_t count) {
      queues.reserve(count);
      for (uint32_t i = 0; i < count; ++i) queues.push_back(std::make_shared<Queue>());
    }

    std::vector<QueuePtr> queues;
  };

  WorkStealingQueueStrategy(const std::shared_ptr<SharedState> &state, uint32_t selfPos)
      : self_(state->queues[selfPos]),
        siblings_(state->queues) {
    siblings_.erase(siblings_.begin() + selfPos);
    std::shuffle(siblings_.begin(), siblings_.end(), folly::ThreadLocalPRNG{});
  }

  void add(T &&item) { self_->add(std::move(item)); }

  size_t queueSize() const { return self_->size(); }

  template <typename EndValueHolder>
  folly::Optional<T> tryTake(std::chrono::milliseconds time, EndValueHolder &endValue) {
    static thread_local size_t called = 0;
    if (LIKELY(called++ % 5 != 4)) {
      if (auto item = self_->try_take_for(time); item) {
        return std::move(item);
      }

      if (auto item = endValue.take(); item) {
        return std::move(item);
      }
    }

    if (!siblings_.empty()) {
      static thread_local size_t next = folly::Random::rand32();
      return siblings_[next++ % siblings_.size()]->try_take();
    }

    return {};
  }

 private:
  QueuePtr self_;
  std::vector<QueuePtr> siblings_;
};

template <typename T>
class RoundRobinQueueStrategy {
 public:
  using ItemType = T;
  using Queue = folly::UnboundedBlockingQueue<T, SimpleSemaphore>;
  using QueuePtr = std::shared_ptr<Queue>;

  struct SharedState {
    explicit SharedState(size_t count) {
      queues.reserve(count);
      for (uint32_t i = 0; i < count; ++i) queues.push_back(std::make_shared<Queue>());
    }

    std::atomic<size_t> next{0};
    std::vector<QueuePtr> queues;
  };

  RoundRobinQueueStrategy(const std::shared_ptr<SharedState> &state, uint32_t selfPos)
      : state_(state),
        selfPos_(selfPos) {
    for (size_t i = 0; i < 4 && i < state->queues.size(); ++i) {
      queues_.push_back(state->queues[(selfPos + i) % state->queues.size()]);
    }
    std::shuffle(queues_.begin() + 1, queues_.end(), folly::ThreadLocalPRNG{});
  }

  void add(T &&item) { queues_[0]->add(std::move(item)); }

  size_t queueSize() const { return queues_[0]->size(); }

  template <typename EndValueHolder>
  folly::Optional<T> tryTake(std::chrono::milliseconds time, EndValueHolder &endValue) {
    static thread_local uint64_t called = 0;
    static thread_local uint64_t next = 0;
    if (LIKELY(called++ % 5 != 4)) {
      for (size_t i = 0; i < queues_.size(); ++i, ++next) {
        auto pos = next % queues_.size();
        if (auto item = queues_[pos]->try_take(); item) {
          return std::move(item);
        }

        if (UNLIKELY(pos == 0 && endValue.hasValue())) {
          return endValue.forceTake();
        }
      }
    }

    ++next;
    return queues_[0]->try_take_for(time);
  }

 private:
  std::shared_ptr<SharedState> state_;
  std::vector<QueuePtr> queues_;
  const size_t selfPos_;
};

// NOTE: BlockingQueueBase should be used as a MPSC queue
template <typename Strategy, typename EndPredicate>
class BlockingQueueBase : public folly::BlockingQueue<typename Strategy::ItemType> {
 public:
  using StrategyType = Strategy;
  using SharedState = typename Strategy::SharedState;
  using T = typename Strategy::ItemType;

  BlockingQueueBase(const std::shared_ptr<SharedState> &state, uint32_t selfPos, EndPredicate isEnd)
      : strategy_(state, selfPos),
        endValue_(std::move(isEnd)) {}

  folly::BlockingQueueAddResult add(T item) override {
    if (!endValue_.add(std::move(item))) {
      strategy_.add(std::move(item));
    }
    return true;
  }

  T take() override {
    for (;;) {
      if (auto item = strategy_.tryTake(timeout_, endValue_); item) {
        return std::move(*item);
      }
    }
    __builtin_unreachable();
  }

  folly::Optional<T> try_take_for(std::chrono::milliseconds time) override {
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + time;
    time = std::min(time, timeout_);
    for (;;) {
      if (auto item = strategy_.tryTake(time, endValue_); item) {
        return std::move(item);
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return {};
      }
    }
    __builtin_unreachable();
  }

  size_t size() override { return strategy_.queueSize(); }

 private:
  static constexpr auto timeout_ = std::chrono::milliseconds(1);

  Strategy strategy_;
  EndValueHolder<EndPredicate, T> endValue_;
};
}  // namespace details

template <typename T, typename EndPredicate>
using SharedNothingBlockingQueue = details::BlockingQueueBase<details::SharedNothingQueueStrategy<T>, EndPredicate>;

template <typename T, typename EndPredicate>
using WorkStealingBlockingQueue = details::BlockingQueueBase<details::WorkStealingQueueStrategy<T>, EndPredicate>;

template <typename T, typename EndPredicate>
using RoundRobinBlockingQueue = details::BlockingQueueBase<details::RoundRobinQueueStrategy<T>, EndPredicate>;
}  // namespace hf3fs
