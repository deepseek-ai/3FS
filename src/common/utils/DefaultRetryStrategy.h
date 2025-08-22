#pragma once

#include <chrono>
#include <folly/experimental/coro/Sleep.h>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/Status.h"
#include "common/utils/UtcTime.h"
#include "fdb/FDBTransaction.h"

namespace hf3fs {
struct CoroSleeper {
  CoTask<void> operator()(std::chrono::milliseconds d) const { co_await folly::coro::sleep(d); }
};

struct RetryConfig {
  using TimeUnit = std::chrono::milliseconds;
  TimeUnit minRetryInterval{0};
  TimeUnit maxRetryInterval{0};
  size_t maxRetryCount{0};
};

struct RetryState {
  using TimeUnit = std::chrono::milliseconds;

  const RetryConfig config;
  TimeUnit retryInterval;
  size_t retryCount = 0;

  RetryState(const RetryConfig &config)
      : config(config),
        retryInterval(config.minRetryInterval) {}

  bool canRetry() const { return retryCount < config.maxRetryCount; }

  void reset() {
    retryInterval = config.minRetryInterval;
    retryCount = 0;
  }

  void next() {
    ++retryCount;
    retryInterval = std::min(2 * retryInterval, config.maxRetryInterval);
  }
};

template <typename Sleeper = CoroSleeper>
class DefaultRetryStrategy {
 public:
  using TimeUnit = std::chrono::milliseconds;
  DefaultRetryStrategy(RetryConfig config, Sleeper sleeper = Sleeper{})
      : state_(config),
        sleeper_(std::move(sleeper)) {}

  template <typename Txn>
  Result<Void> init(Txn * /* txn */) {
    state_.reset();
    return Void{};
  }

  template <typename Txn>
  CoTryTask<void> onError(Txn *txn, Status error) {
    CO_RETURN_ON_ERROR(co_await onError(error));
    txn->reset();
    co_return Void{};
  }

  CoTryTask<void> onError(Status error) {
    if (!kv::TransactionHelper::isRetryable(error, false) || !state_.canRetry()) {
      co_return makeError(std::move(error));
    }
    co_await sleeper_(state_.retryInterval);
    state_.next();
    co_return Void{};
  }

  Sleeper &getSleeper() { return sleeper_; }

 private:
  RetryState state_;
  Sleeper sleeper_;
};
}  // namespace hf3fs
