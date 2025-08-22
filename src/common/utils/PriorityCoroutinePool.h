#pragma once

#include <folly/CancellationToken.h>
#include <folly/Random.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/logging/xlog.h>

#include "common/app/ApplicationBase.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/PriorityUnboundedQueue.h"

namespace hf3fs {

/** Like CoroutinePool, but use PriorityUnboundedQueue in order to support tasks with priority. */
struct PriorityCoroutinePoolConfig : public ConfigBase<PriorityCoroutinePoolConfig> {
  // todo: support hot update coroutine numbers
  CONFIG_HOT_UPDATED_ITEM(coroutines_num, 64u, ConfigCheckers::checkPositive);
  // unused, just for compatibility
  CONFIG_HOT_UPDATED_ITEM(queue_size, 1024u);
  CONFIG_HOT_UPDATED_ITEM(enable_work_stealing, false);
};

template <class Job>
class PriorityCoroutinePool {
 public:
  using Config = PriorityCoroutinePoolConfig;

  PriorityCoroutinePool(const Config &config, uint8_t numPriorities = 3)
      : config_(config),
        queue_(numPriorities),
        cancel_(),
        futures_() {}
  ~PriorityCoroutinePool() { stopAndJoin(); }

  using Handler = std::function<CoTask<void>(Job job)>;
  Result<Void> start(Handler handler, CPUExecutorGroup &grp) {
    auto coroutines = config_.coroutines_num();
    for (auto i = 0u; i < coroutines; ++i) {
      futures_.push_back(run(handler, i).scheduleOn(&grp.pickNext()).start());
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
    XLOGF(INFO, "coroutines pool {} is stopped", fmt::ptr(this));
  }

  void enqueue(Job job, int8_t priority) { queue_.addWithPriority(std::move(job), priority); }

 private:
  CoTask<void> run(Handler handler, size_t queueIndex) {
    while (true) {
      auto result = co_await co_awaitTry(co_withCancellation(cancel_.getToken(), queue_.co_dequeue()));
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

  const Config &config_;
  PriorityUnboundedQueue<Job> queue_;
  CancellationSource cancel_;
  std::vector<folly::SemiFuture<Void>> futures_;
  std::atomic_flag stopped_{false};
};

}  // namespace hf3fs