#include "BackgroundRunner.h"

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/logging/xlog.h>

#include "UtcTime.h"

namespace hf3fs {
BackgroundRunner::BackgroundRunner(folly::Executor::KeepAlive<> executor) {
  XLOGF_IF(FATAL, !executor, "empty executor!");
  executor_ = std::move(executor);
}

BackgroundRunner::BackgroundRunner(CPUExecutorGroup &executor) { executor_ = &executor; }

bool BackgroundRunner::start(String taskName, TaskGetter taskGetter, IntervalGetter intervalGetter) {
  auto lock = std::unique_lock(mutex_);
  if (stopping_) {
    return false;
  }
  if (auto n = latch_.increase(); n == 0) {
    latch_.reset();
  }
  co_withCancellation(cancel_.getToken(), run(std::move(taskName), std::move(taskGetter), std::move(intervalGetter)))
      .scheduleOn(getExecutor())
      .start();
  return true;
}

bool BackgroundRunner::startOnce(String taskName, TaskGetter taskGetter) {
  auto lock = std::unique_lock(mutex_);
  if (stopping_) {
    return false;
  }
  if (auto n = latch_.increase(); n == 0) {
    latch_.reset();
  }
  co_withCancellation(cancel_.getToken(), run(std::move(taskName), std::move(taskGetter), IntervalGetter{}))
      .scheduleOn(getExecutor())
      .start();
  return true;
}

BackgroundRunner::~BackgroundRunner() {
  if (!latch_.tryWait()) {
    folly::coro::blockingWait(stopAll());
  }
  executor_ = (CPUExecutorGroup *)nullptr;
  XLOGF(DBG, "BackgroundRunner: destructed");
}

folly::Executor::KeepAlive<> BackgroundRunner::getExecutor() {
  struct Visitor {
    folly::Executor::KeepAlive<> operator()(folly::Executor::KeepAlive<> executor) const noexcept { return executor; }

    folly::Executor::KeepAlive<> operator()(CPUExecutorGroup *executor) const noexcept { return &executor->pickNext(); }
  };
  return std::visit(Visitor{}, executor_);
}

CoTask<void> BackgroundRunner::stopAll() {
  XLOGF(DBG, "BackgroundRunner: start to execute stopAll");
  {
    auto lock = std::unique_lock(mutex_);
    stopping_ = true;
    cancel_.requestCancellation();
  }
  co_await latch_.wait();
  XLOGF(DBG, "BackgroundRunner: finish to execute stopAll");
}

CoTask<void> BackgroundRunner::run(String taskName, TaskGetter taskGetter, IntervalGetter intervalGetter) {
#define HANDLE_CO_TRY_EXCEPTION()                                                             \
  if (UNLIKELY(res.hasException())) {                                                         \
    if (res.hasException<OperationCancelled>()) {                                             \
      XLOGF(DBG, "BackgroundRunner: {} is cancelled.", taskName);                             \
      break;                                                                                  \
    }                                                                                         \
    XLOGF(FATAL, "BackgroundRunner: {} has exception: {}", taskName, res.exception().what()); \
  }

  XLOGF(INFO, "BackgroundRunner: {} start", taskName);
  for (int64_t i = 1;; ++i) {
    XLOGF(DBG, "BackgroundRunner: {} round {} start", taskName, i);
    auto start = SteadyClock::now();
    auto res = co_await co_awaitTry(taskGetter());
    HANDLE_CO_TRY_EXCEPTION();

    if (!intervalGetter) {
      break;
    }

    auto interval = intervalGetter().asUs();
    if (interval == 0_us) {
      break;
    }

    auto now = SteadyClock::now();
    if (now < start + interval) {
      auto sleepTime = std::chrono::duration_cast<std::chrono::microseconds>(start + interval - now);
      XLOGF(DBG, "BackgroundRunner: {} wait {}us for next round", taskName, sleepTime.count());
      auto res = co_await co_awaitTry(folly::coro::sleep(sleepTime));
      HANDLE_CO_TRY_EXCEPTION();
    }
  }
  // release all captured resources before stopAll finished
  taskGetter = TaskGetter{};
  intervalGetter = IntervalGetter{};

  XLOGF(INFO, "BackgroundRunner: {} stopped", taskName);

  latch_.countDown();

#undef HANDLE_CO_TRY_EXCEPTION
}
}  // namespace hf3fs
