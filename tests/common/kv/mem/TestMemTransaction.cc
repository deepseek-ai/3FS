#include <double-conversion/utils.h>
#include <folly/Conv.h>
#include <folly/Random.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <gtest/gtest.h>
#include <string>
#include <string_view>

#include "common/kv/IKVEngine.h"
#include "common/kv/ITransaction.h"
#include "common/kv/mem/MemKV.h"
#include "common/kv/mem/MemKVEngine.h"
#include "fmt/core.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::kv {

class TestMemTransaction : public ::testing::Test {
 protected:
  constexpr static auto testKey = "unittest.foo";
  constexpr static auto testKey2 = "unittest.foo1";
  constexpr static auto testKey3 = "unittest.foo2";
  constexpr static auto testValue = "unittest.bar";

  CoTryTask<void> setupTestKeys() {
    auto transaction = engine_.createReadWriteTransaction();
    auto result = co_await transaction->set(testKey, testValue);
    CO_RETURN_ON_ERROR(result);
    result = co_await transaction->set(testKey2, testValue);
    CO_RETURN_ON_ERROR(result);
    result = co_await transaction->set(testKey3, testValue);
    CO_RETURN_ON_ERROR(result);
    auto commit = co_await transaction->commit();
    CO_RETURN_ON_ERROR(commit);
    co_return Void();
  }

  void blockingSetupTestKeys() {
    folly::coro::blockingWait([&]() -> CoTask<void> {
      auto result = co_await setupTestKeys();
      EXPECT_TRUE(result.hasValue());
    }());
  }

  MemKVEngine engine_;
};

TEST_F(TestMemTransaction, SetValue) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto oldTransaction = engine_.createReadWriteTransaction();

    auto result = co_await setupTestKeys();
    EXPECT_TRUE(result.hasValue());

    auto readOldResult = co_await oldTransaction->get(testKey);
    EXPECT_TRUE(readOldResult.hasValue());
    EXPECT_FALSE(readOldResult.value().has_value());

    auto newTransaction = engine_.createReadWriteTransaction();
    auto readNewResult = co_await newTransaction->get(testKey);
    EXPECT_TRUE(readNewResult.hasValue());
    EXPECT_TRUE(readNewResult.value().has_value());
  }());
}

TEST_F(TestMemTransaction, SnapshotGet) {
  blockingSetupTestKeys();
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto transaction = engine_.createReadonlyTransaction();
    auto result = co_await transaction->snapshotGet(testKey);
    EXPECT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().has_value());
    EXPECT_EQ(result.value().value(), testValue);
  }());
}

TEST_F(TestMemTransaction, GetRangeSnapshot) {
  blockingSetupTestKeys();
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

TEST_F(TestMemTransaction, GetRangeSnapshot2) {
  blockingSetupTestKeys();
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

TEST_F(TestMemTransaction, GetRangeSnapshot3) {
  blockingSetupTestKeys();
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

TEST_F(TestMemTransaction, Get) {
  blockingSetupTestKeys();
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto transaction = engine_.createReadWriteTransaction();
    auto result = co_await transaction->get(testKey);
    EXPECT_TRUE(result.hasValue());
    EXPECT_TRUE(result.value().has_value());
    EXPECT_EQ(result.value().value(), testValue);
  }());
}

TEST_F(TestMemTransaction, Clear) {
  blockingSetupTestKeys();
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

TEST_F(TestMemTransaction, TransactionNoConflict) {
  constexpr auto M = 8;
  constexpr auto N = 10000;

  blockingSetupTestKeys();

  std::atomic<size_t> conflictTimes{0};
  folly::CPUThreadPoolExecutor executor(M);
  auto parallelSet = [&]() -> CoTask<void> {
    for (auto i = 0; i < M; ++i) {
      auto func = [&]() -> CoTask<void> {
        for (auto j = 0; j < N; ++j) {
          auto tr = engine_.createReadWriteTransaction();
          auto key = std::to_string(j % (N / 100));
          co_await tr->snapshotGet(key);
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

  ASSERT_EQ(conflictTimes, 0);
}

TEST_F(TestMemTransaction, TransactionConflict) {
  constexpr auto M = 8;
  constexpr auto N = 10000;

  enum OP { GET, SNAPSHOT_GET, ADD_READ_CONFLICT };

  blockingSetupTestKeys();
  for (const auto op : {GET, SNAPSHOT_GET, ADD_READ_CONFLICT}) {
    std::atomic<size_t> conflictTimes{0};
    folly::CPUThreadPoolExecutor executor(M);
    auto parallelSet = [&]() -> CoTask<void> {
      for (auto i = 0; i < M; ++i) {
        auto func = [&]() -> CoTask<void> {
          for (auto j = 0; j < N; ++j) {
            auto tr = engine_.createReadWriteTransaction();
            auto key = std::to_string(j % (N / 100));
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

TEST_F(TestMemTransaction, VersionstampedKey) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    std::string_view prefix = "key";
    auto key = fmt::format("{}0000000000", prefix);
    for (size_t i = 0; i < 100; i++) {
      auto tr = engine_.createReadWriteTransaction();
      CO_ASSERT_OK(co_await tr->setVersionstampedKey(key, prefix.size(), folly::to<std::string>(i)));
      CO_ASSERT_OK(co_await tr->commit());
    }
    CO_ASSERT_EQ(prefix, "key");

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

TEST_F(TestMemTransaction, VersionstampedValue) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    std::string key("key");
    std::string buffer(1024, 'a');
    std::string prev;
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

}  // namespace hf3fs::kv
