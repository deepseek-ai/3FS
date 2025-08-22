#include "common/utils/DynamicCoroutinesPool.h"

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Invoke.h>

#include "common/monitor/Recorder.h"

namespace hf3fs {
namespace {

monitor::ValueRecorder currentCoroutinesNum{"common.coroutines_num.current", std::nullopt, false};

}  // namespace

DynamicCoroutinesPool::DynamicCoroutinesPool(const Config &config, std::string_view name)
    : config_(config),
      queue_(config_.queue_size()),
      guard_(config_.addCallbackGuard([&] { setCoroutinesNum(config_.coroutines_num()); })),
      executor_(std::make_pair(config.threads_num(), config.threads_num()),
                std::make_shared<folly::NamedThreadFactory>(name)),
      coroutinesNumRecorder_(currentCoroutinesNum.getRecorderWithTag(monitor::instanceTagSet(std::string{name}))) {}

Result<Void> DynamicCoroutinesPool::start() { return setCoroutinesNum(config_.coroutines_num()); }

Result<Void> DynamicCoroutinesPool::stopAndJoin() {
  if (stopped_.test_and_set()) {
    XLOGF(DBG, "coroutines pool {} is already stopped.", fmt::ptr(this));
    return Void{};
  }
  guard_->dismiss();  // disable coroutines num hot updated.
  RETURN_AND_LOG_ON_ERROR(setCoroutinesNum(0));
  afterCoroutineStop();
  folly::coro::blockingWait(baton_);
  executor_.join();
  return Void{};
}

Result<Void> DynamicCoroutinesPool::setCoroutinesNum(uint32_t num) {
  auto lock = std::unique_lock(mutex_);
  if (executor_.numThreads() != config_.threads_num()) {
    executor_.setNumThreads(config_.threads_num(), true);
  }

  if (num == coroutinesNum_) {
    coroutinesNumRecorder_->set(*currentRunning_.lock());
    return Void{};
  }

  XLOGF(INFO, "pool {} coroutines num {} -> {}", fmt::ptr(this), coroutinesNum_, num);
  if (coroutinesNum_ < num) {
    // increase coroutines num.
    auto inc = num - coroutinesNum_;
    for (; coroutinesNum_ < num; ++coroutinesNum_) {
      run().scheduleOn(&executor_).start();
    }
    auto guard = currentRunning_.lock();
    coroutinesNumRecorder_->set(*guard += inc);
  } else {
    // decrease coroutines num.
    for (; coroutinesNum_ > num; --coroutinesNum_) {
      queue_.enqueue(nullptr);
    }
  }
  return Void{};
}

CoTask<void> DynamicCoroutinesPool::run() {
  SCOPE_EXIT { afterCoroutineStop(); };

  while (true) {
    auto task = co_await queue_.co_dequeue();
    if (task == nullptr) {
      co_return;
    }

    auto result = co_await folly::coro::co_awaitTry(std::move(*task));
    if (UNLIKELY(result.hasException())) {
      XLOGF(FATAL, "DynamicCoroutinesPool has exception: {}", result.exception().what());
      co_return;
    }
  }
}

void DynamicCoroutinesPool::afterCoroutineStop() {
  auto guard = currentRunning_.lock();
  auto now = --*guard;
  coroutinesNumRecorder_->set(now);
  if (now == 0) {
    baton_.post();
  }
}

}  // namespace hf3fs
