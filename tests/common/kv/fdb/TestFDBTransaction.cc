#include <chrono>
#include <cstdint>
#include <folly/CancellationToken.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <gtest/gtest.h>
#include <numeric>
#include <ostream>
#include <set>

#include "FDBTestBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/UtcTime.h"
#include "common/utils/Uuid.h"
#include "fdb/FDBKVEngine.h"
#include "fdb/FDBTransaction.h"
#include "fmt/core.h"
#include "foundationdb/fdb_c_options.g.h"
#include "foundationdb/fdb_c_types.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::kv {

class TestFDBTransaction : public FDBTestBase {
 public:
  void SetUp() override {
    FDBTestBase::SetUp();
    engine_ = FDBKVEngine(std::move(db_));
  }

  Status convertError(fdb_error_t errCode, bool commit) { return FDBTransaction::testFDBError(errCode, commit); }

 protected:
  constexpr static auto testKey = "unittest.foo";
  constexpr static auto testKey2 = "unittest.foo1";
  constexpr static auto testKey3 = "unittest.foo2";
  constexpr static auto testValue = "unittest.bar";
  constexpr static auto conflictKey = "unittest.conflict.";

  FDBKVEngine engine_{std::move(db_)};
};

TEST_F(TestFDBTransaction, SetValue) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto transaction = engine_.createReadWriteTransaction();
    auto result = co_await transaction->set(testKey, testValue);
    EXPECT_TRUE(result.hasValue());
    result = co_await transaction->set(testKey2, testValue);
    EXPECT_TRUE(result.hasValue());
    result = co_await transaction->set(testKey3, testValue);
    EXPECT_TRUE(result.hasValue());
    auto commit = co_await transaction->commit();
    EXPECT_TRUE(commit.hasValue());
  }());
}

TEST_F(TestFDBTransaction, SnapshotGet) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto transaction = engine_.createReadonlyTransaction();
    auto result = co_await transaction->snapshotGet(testKey);
    EXPECT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().has_value());
    EXPECT_EQ(result.value().value(), testValue);
  }());
}

TEST_F(TestFDBTransaction, SnapshotGetSameKey) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto transaction = engine_.createReadonlyTransaction();
    std::set<uint64_t> latency;
    for (size_t i = 0; i < 100; i++) {
      auto begin = SteadyClock::now();
      auto result = co_await transaction->snapshotGet(testKey);
      EXPECT_TRUE(result.hasValue());
      EXPECT_TRUE(result.value().has_value());
      EXPECT_EQ(result.value().value(), testValue);
      auto lat = SteadyClock::now() - begin;
      latency.insert(lat.count() / 1000);
    }
    fmt::print("snapshot get same key, max {}us, min {}us, avg {}us\n",
               *latency.rbegin(),
               *latency.begin(),
               std::accumulate(latency.begin(), latency.end(), 0ul) / latency.size());
  }());
}

TEST_F(TestFDBTransaction, GetRangeSnapshot) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto transaction = engine_.createReadonlyTransaction();
    IReadOnlyTransaction::KeySelector begin(testKey, true);
    IReadOnlyTransaction::KeySelector end(testKey, true);
    auto result = co_await transaction->snapshotGetRange(begin, end, 100);
    EXPECT_TRUE(result.hasValue());

    auto value = std::move(result.value());
    EXPECT_EQ(value.kvs.size(), 1);
    EXPECT_EQ(value.kvs.front().key, testKey);
    EXPECT_FALSE(value.hasMore);
  }());
}

TEST_F(TestFDBTransaction, GetRangeSnapshot2) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto transaction = engine_.createReadonlyTransaction();
    IReadOnlyTransaction::KeySelector begin(testKey, false);
    IReadOnlyTransaction::KeySelector end(testKey3, false);
    auto result = co_await transaction->snapshotGetRange(begin, end, 100);
    EXPECT_TRUE(result.hasValue());

    auto value = std::move(result.value());
    EXPECT_EQ(value.kvs.size(), 1);
    EXPECT_EQ(value.kvs.front().key, testKey2);
    EXPECT_FALSE(value.hasMore);
  }());
}

TEST_F(TestFDBTransaction, GetRangeSnapshot3) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto transaction = engine_.createReadonlyTransaction();
    IReadOnlyTransaction::KeySelector begin(testKey, true);
    IReadOnlyTransaction::KeySelector end(testKey3, true);
    auto result = co_await transaction->snapshotGetRange(begin, end, 2);
    EXPECT_TRUE(result.hasValue());

    auto value = std::move(result.value());
    EXPECT_EQ(value.kvs.size(), 2);
    EXPECT_EQ(value.kvs.front().key, testKey);
    EXPECT_TRUE(value.hasMore);
  }());
}

TEST_F(TestFDBTransaction, Get) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto transaction = engine_.createReadWriteTransaction();
    auto result = co_await transaction->get(testKey);
    EXPECT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().has_value());
    EXPECT_EQ(result.value().value(), testValue);
  }());
}

TEST_F(TestFDBTransaction, Clear) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto transaction = engine_.createReadWriteTransaction();
    auto result = co_await transaction->clear(testKey);
    EXPECT_TRUE(result.hasValue());
    result = co_await transaction->clear(testKey2);
    EXPECT_TRUE(result.hasValue());
    result = co_await transaction->clear(testKey3);
    EXPECT_TRUE(result.hasValue());
    auto commit = co_await transaction->commit();
    EXPECT_TRUE(commit.hasValue());
    auto cancel = co_await transaction->cancel();
    EXPECT_TRUE(cancel.hasValue());
  }());
}

TEST_F(TestFDBTransaction, TransactionConflict) {
  constexpr auto M = 8;
  constexpr auto N = 100;
  enum OP { GET, SNAPSHOT_GET, ADD_READ_CONFLICT };

  for (const auto op : {GET, SNAPSHOT_GET, ADD_READ_CONFLICT}) {
    std::atomic<size_t> conflictTimes{0};
    folly::CPUThreadPoolExecutor executor(M);
    auto parallelSet = [&]() -> CoTask<void> {
      for (auto i = 0; i < M; ++i) {
        auto func = [&]() -> CoTask<void> {
          for (auto j = 0; j < N; ++j) {
            auto tr = engine_.createReadWriteTransaction();
            auto key = conflictKey + std::to_string(j % 100);
            switch (op) {
              case GET:
                co_await tr->get(key);
                break;
              case SNAPSHOT_GET:
                co_await tr->snapshotGet(key);
                break;
              case ADD_READ_CONFLICT:
              default:
                co_await tr->addReadConflict(key);
                break;
            }
            co_await tr->set(key, key);
            auto result = co_await tr->commit();
            if (result.hasError()) {
              if (result.error().code() == TransactionCode::kConflict) {
                ++conflictTimes;
              }
            }
          }
          co_return;
        };
        folly::coro::co_invoke(func).scheduleOn(co_await folly::coro::co_current_executor).start();
      }
    };
    folly::coro::blockingWait(folly::coro::co_invoke(parallelSet).scheduleOn(&executor));
    executor.join();

    fmt::print("Transaction conflict times: {}\n", conflictTimes.load());
    if (op != SNAPSHOT_GET) {
      ASSERT_NE(conflictTimes, 0);
    } else {
      ASSERT_EQ(conflictTimes, 0);
    }
  }
}

TEST_F(TestFDBTransaction, CancellationSafety) {
  folly::CPUThreadPoolExecutor exec(1);
  folly::CancellationSource source;
  auto transaction = engine_.createReadWriteTransaction();
  auto future =
      folly::coro::co_withCancellation(source.getToken(), transaction->get("not found key")).scheduleOn(&exec).start();
  source.requestCancellation();
  ASSERT_THROW(future.wait().value(), folly::OperationCancelled);
}

TEST_F(TestFDBTransaction, TestErrorcode) {
  for (int errCode = 1; errCode < 10000; errCode++) {
    if (errCode == 1039) continue;
    auto status = convertError(errCode, false);
    ASSERT_EQ(TransactionHelper::isRetryable(status, false),
              fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, errCode))
        << status.describe();
    ASSERT_EQ(TransactionHelper::isRetryable(status, true), fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, errCode))
        << status.describe();
  }

  for (int errCode = 1; errCode < 10000; errCode++) {
    auto status = convertError(errCode, true);
    if (errCode == 1039) continue;
    ASSERT_EQ(TransactionHelper::isRetryable(status, false),
              fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, errCode))
        << status.describe();
    ASSERT_EQ(TransactionHelper::isRetryable(status, true), fdb_error_predicate(FDB_ERROR_PREDICATE_RETRYABLE, errCode))
        << status.describe();
  }
}

TEST_F(TestFDBTransaction, VersionstampedKey) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto prefix = fmt::format("test.{}", Uuid::random());
    auto key = fmt::format("{}0000000000", prefix);
    for (size_t i = 0; i < 100; i++) {
      auto tr = engine_.createReadWriteTransaction();
      CO_ASSERT_OK(co_await tr->setVersionstampedKey(key, prefix.size(), folly::to<std::string>(i)));
      CO_ASSERT_OK(co_await tr->commit());
    }

    auto tr = engine_.createReadonlyTransaction();
    auto result = co_await tr->getRange({prefix, true}, {TransactionHelper::prefixListEndKey(prefix), false}, 120);
    CO_ASSERT_OK(result);
    CO_ASSERT_EQ(result->kvs.size(), 100);
    for (size_t i = 0; i < 100; i++) {
      auto [key, value] = result->kvs.at(i).pair();
      CO_ASSERT_EQ(key.substr(0, prefix.size()), prefix);
      CO_ASSERT_EQ(key.size(), prefix.size() + 10);
      CO_ASSERT_EQ(value, folly::to<std::string>(i));
    }
  }());
}

TEST_F(TestFDBTransaction, VersionstampedValue) {
  std::string prev;
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto key = fmt::format("test.{}-", Uuid::random());
    std::string buffer(1024, 'a');
    for (size_t i = 0; i < 100; i++) {
      auto offset = folly::Random::rand32(1024 - 10);
      {
        auto tr = engine_.createReadWriteTransaction();
        CO_ASSERT_OK(co_await tr->setVersionstampedValue(key, buffer, offset));
        CO_ASSERT_OK(co_await tr->commit());
      }

      {
        auto tr = engine_.createReadonlyTransaction();
        auto value = co_await tr->get(key);
        CO_ASSERT_OK(value);
        CO_ASSERT_TRUE(value->has_value());
        auto str = **value;
        CO_ASSERT_EQ(str.substr(0, offset), buffer.substr(0, offset));
        CO_ASSERT_GE(str.substr(offset, 10), prev);
        CO_ASSERT_EQ(str.substr(offset + 10), buffer.substr(offset + 10));
        prev = str.substr(offset, 10);
      }
    }
  }());
}

TEST_F(TestFDBTransaction, Readonly) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    engine_.setReadonly(true);
    SCOPE_EXIT { engine_.setReadonly(false); };
    auto transaction = engine_.createReadWriteTransaction();
    auto commit = co_await transaction->commit();
    EXPECT_TRUE(commit.hasError());
  }());
}

}  // namespace hf3fs::kv
