#pragma once

#include <folly/Random.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/Sleep.h>

#include "common/utils/BoundedQueue.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"

namespace hf3fs {

struct CoroutinesPoolBase {
  class Config : public ConfigBase<Config> {
    CONFIG_ITEM(coroutines_num, 64u);
    CONFIG_ITEM(queue_size, 1024u);
    CONFIG_ITEM(enable_work_stealing, false);

   public:
    Config() = default;
    Config(uint32_t coroutinesNum, uint32_t queueSize, bool enableWorkStealing = false) {
      set_coroutines_num(coroutinesNum);
      set_queue_size(queueSize);
      set_enable_work_stealing(enableWorkStealing);
    }
  };
};

template <class Job>
class CoroutinesPool : public CoroutinesPoolBase {
 public:
  CoroutinesPool(const Config &config, std::optional<folly::CPUThreadPoolExecutor::KeepAlive<>> executor = std::nullopt)
      : config_(config),
        executor_(std::move(executor)) {
    if (config_.enable_work_stealing()) {
      auto coroutinesNum = config_.coroutines_num();
      auto queueSizePerCoroutine = (config_.queue_size() + coroutinesNum - 1) / coroutinesNum;
      for (auto i = 0u; i < coroutinesNum; ++i) {
        funcQueues_.push_back(std::make_unique<Queue>(queueSizePerCoroutine));
      }
    } else {
      funcQueues_.push_back(std::make_unique<Queue>(config_.queue_size()));
    }
  }
  ~CoroutinesPool() { stopAndJoin(); }

  using Handler = std::function<CoTask<void>(Job job)>;
  Result<Void> start(Handler handler) {
    if (UNLIKELY(!executor_.has_value())) {
      auto msg = fmt::format("CoroutinesPool@{} does not have an executor", fmt::ptr(this));
      XLOG(ERR, msg);
      return makeError(StatusCode::kInvalidConfig, std::move(msg));
    }
    auto coroutinesNum = config_.coroutines_num();
    for (auto i = 0u; i < coroutinesNum; ++i) {
      futures_.push_back(run(handler, i).scheduleOn(*executor_).start());
    }
    return Void{};
  }

  Result<Void> start(Handler handler, CPUExecutorGroup &executor) {
    auto coroutinesNum = config_.coroutines_num();
    for (auto i = 0u; i < coroutinesNum; ++i) {
      futures_.push_back(run(handler, i).scheduleOn(&executor.pickNext()).start());
    }
    return Void{};
  }

  void stopAndJoin() {
    if (stopped_.test_and_set()) {
      XLOGF(DBG, "coroutines pool {} is already stopped.", fmt::ptr(this));
      return;
    }
    cancel_.requestCancellation();
    for (auto &future : futures_) {
      future.wait();
    }
    futures_.clear();
    if (executor_.has_value()) {
      executor_->reset();
    }
    XLOGF(INFO, "coroutines pool {} is stopped", fmt::ptr(this));
  }

  void enqueueSync(Job job) { pickQueue().enqueue(std::move(job)); }
  CoTask<void> enqueue(Job job) { co_await pickQueue().co_enqueue(std::move(job)); }

 protected:
  using Queue = BoundedQueue<Job>;
  using QueuePtr = std::unique_ptr<Queue>;

  Queue &pickQueue() {
    auto n = folly::Random::rand32(0, funcQueues_.size());
    return *funcQueues_[n];
  }

  struct WorkStealingJobTaker {
    WorkStealingJobTaker(bool enableWorkStealing,
                         size_t selfIndex,
                         size_t totalRunners,
                         const std::vector<QueuePtr> &qs)
        : self(enableWorkStealing ? selfIndex : 0),
          queues(qs) {
      if (enableWorkStealing) {
        for (size_t i = 0; i < totalRunners; ++i) {
          if (i != selfIndex) {
            others.push_back(i);
          }
        }
        std::shuffle(others.begin(), others.end(), folly::ThreadLocalPRNG{});
      }
    }

    CoTask<Job> take() {
      if (others.empty()) {
        co_return co_await queues[self]->co_dequeue();
      }

      while (true) {
        if (auto maybeJob = co_await timedWait(*queues[self]); maybeJob) {
          co_return std::move(*maybeJob);
        }

        auto otherIndex = others[nextIndex++ % others.size()];
        if (auto maybeJob = queues[otherIndex]->try_dequeue(); maybeJob) {
          co_return std::move(*maybeJob);
        }
      }
      __builtin_unreachable();
    }

    CoTask<std::optional<Job>> timedWait(Queue &queue) {
      static constexpr auto timeout = std::chrono::milliseconds(5);
      auto [sleepResult, dequeueResult] =
          co_await folly::coro::collectAnyNoDiscard(folly::coro::sleep(timeout), queue.co_dequeue());
      if (!dequeueResult.hasValue() && !dequeueResult.hasException()) {
        co_return std::nullopt;
      }
      co_return std::move(*dequeueResult);
    }

    size_t self;
    size_t nextIndex = 0;
    std::vector<size_t> others;
    const std::vector<QueuePtr> &queues;
  };

  CoTask<void> run(Handler handler, size_t queueIndex) {
    WorkStealingJobTaker taker(config_.enable_work_stealing(), queueIndex, config_.coroutines_num(), funcQueues_);
    while (true) {
      auto result = co_await co_awaitTry(co_withCancellation(cancel_.getToken(), taker.take()));
      if (UNLIKELY(result.template hasException<OperationCancelled>())) {
        XLOGF(DBG, "a coroutine in pool {} is stopped", fmt::ptr(this));
        break;
      } else if (result.hasException()) {
        XLOGF(ERR, "a coroutine in pool {} is stopped (exception: {})", fmt::ptr(this), result.exception().what());
        break;
      }
      co_await handler(std::move(*result));
    }
  }

 private:
  const Config &config_;
  std::optional<folly::CPUThreadPoolExecutor::KeepAlive<>> executor_;
  std::vector<QueuePtr> funcQueues_;

  CancellationSource cancel_;
  std::vector<folly::SemiFuture<Void>> futures_;
  std::atomic_flag stopped_{false};
};

}  // namespace hf3fs
