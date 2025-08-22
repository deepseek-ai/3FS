#pragma once

#include <algorithm>
#include <chrono>
#include <folly/Likely.h>
#include <folly/Random.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/symbolizer/Symbolizer.h>
#include <folly/logging/xlog.h>
#include <foundationdb/fdb_c_types.h>
#include <string_view>
#include <utility>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "common/utils/Status.h"
#include "fdb/FDBTransaction.h"

namespace hf3fs::kv {

class FDBRetryStrategy {
 public:
  static constexpr Duration kMinBackoff = 10_ms;  // same with FDB, 0.01s

  struct Config {
    Duration maxBackoff = 1_s;
    size_t maxRetryCount = 10;
    bool retryMaybeCommitted = true;
  };

  FDBRetryStrategy()
      : FDBRetryStrategy(Config()) {}
  FDBRetryStrategy(Config config)
      : config_(config),
        backoff_(kMinBackoff),
        retry_(0) {}

  template <typename Txn>
  Result<Void> init(Txn *txn) {
    retry_ = 0;
    backoff_ = kMinBackoff;

    auto *fdbTransaction = dynamic_cast<FDBTransaction *>(txn);
    if (fdbTransaction) {
      uint64_t value = std::chrono::duration_cast<std::chrono::milliseconds>(config_.maxBackoff).count();
      auto result = fdbTransaction->setOption(FDBTransactionOption::FDB_TR_OPTION_MAX_RETRY_DELAY,
                                              std::string_view((char *)&value, sizeof(value)));
      if (result.hasError()) {
        XLOGF(ERR, "Failed to set option on FDBTransaction, {}", result.error().describe());
        RETURN_ERROR(result);
      }
    }

    return Void{};
  }

  template <typename Txn>
  CoTryTask<void> onError(Txn *txn, Status error) {
    if (retry_ >= config_.maxRetryCount) {
      XLOGF(ERR, "Transaction failed after retry {} times, error {}", retry_, error.describe());
      co_return makeError(std::move(error));
    }
    if (!TransactionHelper::isTransactionError(error)) {
      XLOGF(DBG, "Not transaction error {}", error);
      co_return makeError(std::move(error));
    }
    XLOGF(DBG, "Transaction error {}", error);

    SCOPE_EXIT {
      // update retry and backoff before exit.
      backoff_ = std::min(config_.maxBackoff, Duration(backoff_ * 2));
      retry_++;
    };

    auto *fdbTransaction = dynamic_cast<FDBTransaction *>(txn);
    if (fdbTransaction) {
      co_return co_await fdbBackoff(fdbTransaction, std::move(error));
    } else {
      co_return co_await defaultBackoff(txn, std::move(error));
    }
  }

  CoTryTask<void> fdbBackoff(FDBTransaction *txn, Status error) {
    auto errcode = txn->errcode();
    if (UNLIKELY(!errcode)) {
      XLOGF_IF(CRITICAL,
               error.code() != TransactionCode::kTooOld,
               "Failed to get FDB errcode, error {}, stacktrace {}",
               error,
               folly::symbolizer::getStackTraceStr());
      co_return co_await defaultBackoff(txn, std::move(error));
    }

    FDBErrorPredicate predict =
        config_.retryMaybeCommitted ? FDB_ERROR_PREDICATE_RETRYABLE : FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED;
    if (!fdb_error_predicate(predict, errcode)) {
      XLOGF(ERR, "Transaction error not retryable: {}, errcode {}", error, errcode);
      co_return makeError(std::move(error));
    }

    XLOGF(DBG, "FDBRetryStrategy backoff by FoundationDB");
    auto ok = co_await txn->onError(errcode);
    if (!ok) {
      co_return makeError(std::move(error));
    }
    co_return Void{};
  }

  template <typename Txn>
  CoTryTask<void> defaultBackoff(Txn *txn, Status error) {
    // fallback to our backoff implementation
    if (!TransactionHelper::isRetryable(error, config_.retryMaybeCommitted)) {
      XLOGF(ERR, "Transaction error not retryable: {}", error);
      co_return makeError(std::move(error));
    }
    XLOGF(WARN, "Transaction retryable error: {}", error);

    XLOGF(DBG, "FDBRetryStrategy backoff transaction {}ms", backoff_.count());
    txn->reset();

    auto duration = Duration(backoff_ / 100 * folly::Random::rand32(80, 120));
    co_await folly::coro::sleep(duration.asUs());
    co_return Void{};
  }

 private:
  Config config_;
  Duration backoff_;
  size_t retry_;
};

}  // namespace hf3fs::kv