#include <folly/experimental/coro/BlockingWait.h>
#include <gtest/gtest.h>
#include <string_view>

#include "common/kv/IKVEngine.h"
#include "common/kv/WithTransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/DefaultRetryStrategy.h"
#include "common/utils/Result.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::tests {
namespace {
using namespace ::hf3fs::kv;
using namespace std::chrono_literals;

struct MockSleeper {
  CoTask<void> operator()(std::chrono::milliseconds d) {
    records.push_back(d);
    co_return;
  }

  std::vector<std::chrono::milliseconds> records;
};

DefaultRetryStrategy<MockSleeper> getDefaultRetryStrategy() {
  return DefaultRetryStrategy<MockSleeper>(RetryConfig{10ms, 100ms, 5});
}

struct OpResultSeq : public std::vector<uint32_t> {
  using Base = std::vector<uint32_t>;
  using Base::Base;

  size_t cur_ = 0;

  explicit OpResultSeq(Base req)
      : Base(std::move(req)) {}

  CoTryTask<void> next() {
    auto code = at(cur_++);
    if (code == StatusCode::kOK) {
      co_return Void();
    }
    co_return makeError(code);
  }

  std::string_view peak() { return StatusCode::toString(at(cur_)); }
};

class MockROTxn : public IReadOnlyTransaction {
 public:
  CoTryTask<std::optional<String>> snapshotGet(std::string_view) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<GetRangeResult> snapshotGetRange(const KeySelector &, const KeySelector &, int32_t) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<GetRangeResult> getRange(const KeySelector &, const KeySelector &, int32_t) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<void> cancel() override { co_return makeError(StatusCode::kNotImplemented); }
  void reset() override {}

  void setReadVersion(int64_t) override {}
};

class MockRWTxn : public IReadWriteTransaction {
 public:
  MockRWTxn(OpResultSeq &commitSeq)
      : commitSeq_(commitSeq) {}

  CoTryTask<std::optional<String>> snapshotGet(std::string_view) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<GetRangeResult> snapshotGetRange(const KeySelector &, const KeySelector &, int32_t) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<std::optional<String>> get(std::string_view) override { co_return makeError(StatusCode::kNotImplemented); }

  CoTryTask<GetRangeResult> getRange(const KeySelector &, const KeySelector &, int32_t) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<void> addReadConflict(std::string_view) override { co_return makeError(StatusCode::kNotImplemented); }
  CoTryTask<void> addReadConflictRange(std::string_view, std::string_view) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<void> set(std::string_view, std::string_view) override { co_return makeError(StatusCode::kNotImplemented); }

  CoTryTask<void> clear(std::string_view) override { co_return makeError(StatusCode::kNotImplemented); }

  CoTryTask<void> setVersionstampedKey(std::string_view key, uint32_t offset, std::string_view value) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<void> setVersionstampedValue(std::string_view key, std::string_view value, uint32_t offset) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<void> commit() override { co_return co_await commitSeq_.next(); }

  CoTryTask<void> cancel() override { co_return makeError(StatusCode::kNotImplemented); }

  int64_t getCommittedVersion() override { return -1; }

  void setReadVersion(int64_t) override {}

  void reset() override {}

 private:
  OpResultSeq &commitSeq_;
};

struct MockHandler {
  explicit MockHandler(OpResultSeq &seq)
      : seq_(seq) {}

  CoTryTask<int> operator()(IReadOnlyTransaction &) {
    auto result = co_await seq_.next();
    CO_RETURN_ON_ERROR(result);
    co_return 10;
  }

  CoTryTask<void> operator()(IReadWriteTransaction &) {
    auto result = co_await seq_.next();
    CO_RETURN_ON_ERROR(result);
    co_return Void();
  }

  OpResultSeq &seq_;
};

struct RetryStrategyRef {
  DefaultRetryStrategy<MockSleeper> &ref;

  template <typename Txn>
  Result<Void> init(Txn *txn) {
    return ref.init(txn);
  }

  template <typename Txn>
  CoTryTask<void> onError(Txn *txn, Status error) {
    co_return co_await ref.onError(txn, error);
  }
};

CoTask<std::pair<Result<int>, DefaultRetryStrategy<MockSleeper>>> handleROTxn(OpResultSeq &opSeq) {
  auto handler = MockHandler(opSeq);
  auto retryStrategy = getDefaultRetryStrategy();
  MockROTxn txn;
  auto result = co_await WithTransaction<RetryStrategyRef>({retryStrategy}).run(txn, handler);
  co_return std::make_pair<Result<int>, DefaultRetryStrategy<MockSleeper>>(std::move(result), std::move(retryStrategy));
}

CoTask<std::pair<Result<Void>, DefaultRetryStrategy<MockSleeper>>> handleRWTxn(OpResultSeq &opSeq,
                                                                               OpResultSeq &commitSeq) {
  auto handler = MockHandler(opSeq);
  auto retryStrategy = getDefaultRetryStrategy();
  MockRWTxn txn(commitSeq);
  auto result = co_await WithTransaction<RetryStrategyRef>({retryStrategy}).run(txn, handler);
  co_return std::make_pair<Result<Void>, DefaultRetryStrategy<MockSleeper>>(std::move(result),
                                                                            std::move(retryStrategy));
}

TEST(WithTransaction, testReadOnly) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    {
      // normal succeed
      OpResultSeq opSeq = {StatusCode::kOK};
      auto [result, retryStrategy] = co_await handleROTxn(opSeq);
      CO_ASSERT_OK(result);
      std::vector<std::chrono::milliseconds> expected = {};
      CO_ASSERT_EQ(retryStrategy.getSleeper().records, expected);
    }
    {
      // op retry once
      OpResultSeq opSeq = {TransactionCode::kThrottled, StatusCode::kOK};
      auto [result, retryStrategy] = co_await handleROTxn(opSeq);
      CO_ASSERT_OK(result);
      std::vector<std::chrono::milliseconds> expected = {10ms};
      CO_ASSERT_EQ(retryStrategy.getSleeper().records, expected);
    }
    {
      // op fail, cancel fail (IReadOnlyTransaction allows to ignore cancel fail), op fail, cancel fail again, op ok
      OpResultSeq opSeq = {TransactionCode::kThrottled, TransactionCode::kTooOld, StatusCode::kOK};
      auto [result, retryStrategy] = co_await handleROTxn(opSeq);
      CO_ASSERT_OK(result);
      std::vector<std::chrono::milliseconds> expected = {10ms, 20ms};
      CO_ASSERT_EQ(retryStrategy.getSleeper().records, expected);
    }
    {
      // op could fail at most 5 times then succeed
      OpResultSeq opSeq(5, TransactionCode::kThrottled);
      opSeq.push_back(StatusCode::kOK);
      auto [result, retryStrategy] = co_await handleROTxn(opSeq);
      CO_ASSERT_OK(result);
      std::vector<std::chrono::milliseconds> expected = {10ms, 20ms, 40ms, 80ms, 100ms};
      CO_ASSERT_EQ(retryStrategy.getSleeper().records, expected);
    }
    {
      // op failed 6 times and return error
      OpResultSeq opSeq(6, TransactionCode::kThrottled);
      opSeq.push_back(StatusCode::kOK);
      auto [result, retryStrategy] = co_await handleROTxn(opSeq);
      CO_ASSERT_TRUE(result.hasError());
      CO_ASSERT_EQ(result.error().code(), TransactionCode::kThrottled);
      std::vector<std::chrono::milliseconds> expected = {10ms, 20ms, 40ms, 80ms, 100ms};
      CO_ASSERT_EQ(retryStrategy.getSleeper().records, expected);
    }
  }());
}

TEST(WithTransaction, testReadWrite) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    {
      // normal succeed
      OpResultSeq seq = {StatusCode::kOK};
      OpResultSeq txnCommitSeq = {StatusCode::kOK};
      auto [result, retryStrategy] = co_await handleRWTxn(seq, txnCommitSeq);
      CO_ASSERT_OK(result);
      std::vector<std::chrono::milliseconds> expected = {};
      CO_ASSERT_EQ(retryStrategy.getSleeper().records, expected);
    }
    {
      // op retry once
      OpResultSeq opSeq = {TransactionCode::kThrottled, StatusCode::kOK};
      OpResultSeq txnCommitSeq = {StatusCode::kOK};
      auto [result, retryStrategy] = co_await handleRWTxn(opSeq, txnCommitSeq);
      CO_ASSERT_OK(result);
      std::vector<std::chrono::milliseconds> expected = {10ms};
      CO_ASSERT_EQ(retryStrategy.getSleeper().records, expected);
    }
    {
      // op succeed, commit fail, retry op and commit succeed
      OpResultSeq opSeq = {StatusCode::kOK, StatusCode::kOK};
      OpResultSeq txnCommitSeq = {TransactionCode::kThrottled, StatusCode::kOK};
      auto [result, retryStrategy] = co_await handleRWTxn(opSeq, txnCommitSeq);
      CO_ASSERT_OK(result);
      std::vector<std::chrono::milliseconds> expected = {10ms};
      std::cout << retryStrategy.getSleeper().records.at(0).count() << std::endl;
      CO_ASSERT_EQ(retryStrategy.getSleeper().records, expected);
    }
    {
      // op could fail at most 5 times then succeed
      OpResultSeq opSeq(5, TransactionCode::kThrottled);
      opSeq.push_back(StatusCode::kOK);
      OpResultSeq txnCommitSeq = {StatusCode::kOK};
      auto [result, retryStrategy] = co_await handleRWTxn(opSeq, txnCommitSeq);
      CO_ASSERT_OK(result);
      std::vector<std::chrono::milliseconds> expected = {10ms, 20ms, 40ms, 80ms, 100ms};
      CO_ASSERT_EQ(retryStrategy.getSleeper().records, expected);
    }
    {
      // op failed 6 times and return error
      OpResultSeq opSeq(6, TransactionCode::kThrottled);
      opSeq.push_back(StatusCode::kOK);
      OpResultSeq txnCommitSeq;
      auto [result, retryStrategy] = co_await handleRWTxn(opSeq, txnCommitSeq);
      CO_ASSERT_TRUE(result.hasError());
      CO_ASSERT_EQ(result.error().code(), TransactionCode::kThrottled);
      std::vector<std::chrono::milliseconds> expected = {10ms, 20ms, 40ms, 80ms, 100ms};
      CO_ASSERT_EQ(retryStrategy.getSleeper().records, expected);
    }
  }());
}

}  // namespace
}  // namespace hf3fs::tests
