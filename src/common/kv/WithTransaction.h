#pragma once

#include <folly/Utility.h>
#include <folly/logging/xlog.h>
#include <memory.h>
#include <memory>
#include <type_traits>
#include <utility>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"

namespace hf3fs::kv {

template <typename RetryStrategy>
class WithTransaction : public folly::MoveOnly {
 public:
  WithTransaction(RetryStrategy strategy)
      : strategy_(std::move(strategy)) {}

  template <typename Handler>
  std::invoke_result_t<Handler, IReadOnlyTransaction &> run(std::unique_ptr<IReadOnlyTransaction> txn,
                                                            Handler &&handler) {
    co_return co_await run(*txn, std::forward<Handler>(handler));
  }

  template <typename Handler>
  std::invoke_result_t<Handler, IReadWriteTransaction &> run(std::unique_ptr<IReadWriteTransaction> txn,
                                                             Handler &&handler) {
    co_return co_await run(*txn, std::forward<Handler>(handler));
  }

  template <typename Handler>
  std::invoke_result_t<Handler, IReadOnlyTransaction &> run(IReadOnlyTransaction &txn, Handler &&handler) {
    auto result = strategy_.init(&txn);
    CO_RETURN_ON_ERROR(result);

    while (true) {
      auto result = co_await handler(txn);
      if (!result.hasError()) {
        co_return std::move(result);
      }
      auto retryResult = co_await strategy_.onError(&txn, std::move(result.error()));
      CO_RETURN_ON_ERROR(retryResult);
    }
  }

  template <typename Handler>
  std::invoke_result_t<Handler, IReadWriteTransaction &> run(IReadWriteTransaction &txn, Handler &&handler) {
    auto result = strategy_.init(&txn);
    CO_RETURN_ON_ERROR(result);

    while (true) {
      auto result = co_await runAndCommit(txn, std::forward<Handler>(handler));
      if (!result.hasError()) {
        co_return std::move(result);
      }
      auto retryResult = co_await strategy_.onError(&txn, std::move(result.error()));
      CO_RETURN_ON_ERROR(retryResult);
    }
  }

  RetryStrategy &getStrategy() { return strategy_; }

 private:
  template <typename Handler>
  std::invoke_result_t<Handler, IReadWriteTransaction &> runAndCommit(IReadWriteTransaction &txn, Handler &&handler) {
    auto result = co_await handler(txn);
    if (!result.hasError()) {
      auto commitResult = co_await txn.commit();
      CO_RETURN_ON_ERROR(commitResult);
    }
    co_return std::move(result);
  }

  RetryStrategy strategy_;
};

}  // namespace hf3fs::kv
