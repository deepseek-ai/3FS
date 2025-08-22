#include <cstdint>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <optional>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include "FDBTestBase.h"

namespace hf3fs::kv {

class TestFDB : public FDBTestBase {};

TEST_F(TestFDB, WaitFuture) {
  ASSERT_EQ(db_.error(), 0);

  folly::coro::blockingWait([this]() -> folly::coro::Task<void> {
    fdb::Transaction tr(db_);
    auto sizeResult = co_await tr.getApproximateSize();
    EXPECT_TRUE(sizeResult.error() == 0);

    auto result = co_await tr.get("foo", true);
    EXPECT_TRUE(result.error() == 0);
    EXPECT_FALSE(result.value().has_value());
  }());
}

TEST_F(TestFDB, GetRange) {
  ASSERT_EQ(db_.error(), 0);

  ::FDBTransaction *tr = nullptr;

  std::string startKey = "unittest.get_range";
  std::string endKey = "unittest.get_rangez";

  // 1. set
  {
    ASSERT_EQ(fdb_database_create_transaction(db_, &tr), 0);
    for (int i = 0; i < 100; ++i) {
      auto k = startKey + fmt::format("{:02d}", i);
      auto v = fmt::format("unittest.value{:02d}", i);
      fdb_transaction_set(tr, (const uint8_t *)k.data(), k.length(), (const uint8_t *)v.data(), v.length());
    }
    auto f = fdb_transaction_commit(tr);
    ASSERT_EQ(fdb_future_block_until_ready(f), 0);
    ASSERT_EQ(fdb_future_get_error(f), 0);
    fdb_future_destroy(f);
    fdb_transaction_destroy(tr);
  }

  // 2. get range
  {
    ASSERT_EQ(fdb_database_create_transaction(db_, &tr), 0);
    auto f =
        fdb_transaction_get_range(tr,
                                  FDB_KEYSEL_FIRST_GREATER_OR_EQUAL((const uint8_t *)startKey.data(), startKey.size()),
                                  FDB_KEYSEL_FIRST_GREATER_THAN((const uint8_t *)endKey.data(), endKey.size()),
                                  10,
                                  0,
                                  FDB_STREAMING_MODE_SERIAL,
                                  0,
                                  false,
                                  false);
    ASSERT_EQ(fdb_future_block_until_ready(f), 0);
    ASSERT_EQ(fdb_future_get_error(f), 0);

    const FDBKeyValue *kv = nullptr;
    int count = 0;
    fdb_bool_t more = false;
    ASSERT_EQ(fdb_future_get_keyvalue_array(f, &kv, &count, &more), 0);
    ASSERT_TRUE(more);
    ASSERT_EQ(count, 10);

    fdb_future_destroy(f);
    fdb_transaction_destroy(tr);
  }

  {
    ASSERT_EQ(fdb_database_create_transaction(db_, &tr), 0);
    auto f =
        fdb_transaction_get_range(tr,
                                  FDB_KEYSEL_FIRST_GREATER_OR_EQUAL((const uint8_t *)startKey.data(), startKey.size()),
                                  FDB_KEYSEL_FIRST_GREATER_THAN((const uint8_t *)endKey.data(), endKey.size()),
                                  0,
                                  0,
                                  FDB_STREAMING_MODE_SERIAL,
                                  0,
                                  false,
                                  false);
    ASSERT_EQ(fdb_future_block_until_ready(f), 0);
    ASSERT_EQ(fdb_future_get_error(f), 0);

    const FDBKeyValue *kv = nullptr;
    int count = 0;
    fdb_bool_t more = false;
    ASSERT_EQ(fdb_future_get_keyvalue_array(f, &kv, &count, &more), 0);
    ASSERT_FALSE(more);
    ASSERT_EQ(count, 100);

    fdb_future_destroy(f);
    fdb_transaction_destroy(tr);
  }

  {
    ASSERT_EQ(fdb_database_create_transaction(db_, &tr), 0);
    auto f =
        fdb_transaction_get_range(tr,
                                  FDB_KEYSEL_FIRST_GREATER_OR_EQUAL((const uint8_t *)startKey.data(), startKey.size()),
                                  FDB_KEYSEL_FIRST_GREATER_THAN((const uint8_t *)endKey.data(), endKey.size()),
                                  -1,
                                  0,
                                  FDB_STREAMING_MODE_SERIAL,
                                  0,
                                  false,
                                  false);
    ASSERT_EQ(fdb_future_block_until_ready(f), 0);
    ASSERT_EQ(fdb_future_get_error(f), 0);

    const FDBKeyValue *kv = nullptr;
    int count = 0;
    fdb_bool_t more = false;
    ASSERT_EQ(fdb_future_get_keyvalue_array(f, &kv, &count, &more), 0);
    ASSERT_FALSE(more);
    ASSERT_EQ(count, 100);

    fdb_future_destroy(f);
    fdb_transaction_destroy(tr);
  }
  // 3. clear
  {
    ASSERT_EQ(fdb_database_create_transaction(db_, &tr), 0);
    for (int i = 0; i < 100; ++i) {
      auto k = startKey + fmt::format("{:02d}", i);
      fdb_transaction_clear(tr, (const uint8_t *)k.data(), k.length());
    }
    auto f = fdb_transaction_commit(tr);
    ASSERT_EQ(fdb_future_block_until_ready(f), 0);
    ASSERT_EQ(fdb_future_get_error(f), 0);
    fdb_future_destroy(f);
    fdb_transaction_destroy(tr);
  }
}

TEST_F(TestFDB, Version) {
  ASSERT_EQ(db_.error(), 0);

  for (auto delay : {0, 10, 100, 200, 500, 1000}) {
    ::FDBTransaction *tr1 = nullptr;
    ::FDBTransaction *tr2 = nullptr;
    ASSERT_EQ(fdb_database_create_transaction(db_, &tr1), 0);
    ASSERT_EQ(fdb_database_create_transaction(db_, &tr2), 0);
    std::string_view key = "random-key";
    fdb_transaction_clear(tr1, (uint8_t *)key.data(), key.size());
    fdb_transaction_clear(tr2, (uint8_t *)key.data(), key.size());
    auto f1 = fdb_transaction_commit(tr1);
    if (delay) usleep(delay);
    auto f2 = fdb_transaction_commit(tr2);
    ASSERT_EQ(fdb_future_block_until_ready(f1), 0);
    ASSERT_EQ(fdb_future_get_error(f1), 0);
    ASSERT_EQ(fdb_future_block_until_ready(f2), 0);
    ASSERT_EQ(fdb_future_get_error(f2), 0);
    int64_t v1, v2;
    ASSERT_EQ(fdb_transaction_get_committed_version(tr1, &v1), 0);
    ASSERT_EQ(fdb_transaction_get_committed_version(tr2, &v2), 0);
    fmt::print("delay {}us, v1 {}, v2 {}, v1 == v2 {}\n", delay, v1, v2, v1 == v2);
    fdb_future_destroy(f1);
    fdb_transaction_destroy(tr1);
    fdb_future_destroy(f2);
    fdb_transaction_destroy(tr2);
  }

  ::FDBTransaction *tr = nullptr;
  std::string_view key = "test-version";
  std::vector<std::pair<int64_t, std::optional<std::string_view>>> vec;
  for (auto value :
       std::vector<std::optional<std::string_view>>{std::nullopt, std::optional("value1"), std::optional("value2")}) {
    ASSERT_EQ(fdb_database_create_transaction(db_, &tr), 0);
    if (value) {
      fdb_transaction_set(tr, (uint8_t *)key.data(), key.size(), (uint8_t *)value->data(), value->size());
    } else {
      fdb_transaction_clear(tr, (uint8_t *)key.data(), key.size());
    }
    int64_t version;
    auto f = fdb_transaction_commit(tr);
    ASSERT_EQ(fdb_future_block_until_ready(f), 0);
    ASSERT_EQ(fdb_future_get_error(f), 0);
    fdb_future_destroy(f);
    ASSERT_EQ(fdb_transaction_get_committed_version(tr, &version), 0);
    fdb_transaction_destroy(tr);
    vec.push_back({version, value});
  }

  for (auto [version, expected] : vec) {
    ASSERT_EQ(fdb_database_create_transaction(db_, &tr), 0);
    fdb_transaction_set_read_version(tr, version);
    auto f = fdb_transaction_get(tr, (uint8_t *)key.data(), key.size(), true);
    ASSERT_EQ(fdb_future_block_until_ready(f), 0);
    ASSERT_EQ(fdb_future_get_error(f), 0);
    const uint8_t *value = nullptr;
    int length = 0;
    fdb_bool_t present = false;
    ASSERT_EQ(fdb_future_get_value(f, &present, &value, &length), 0);
    if (!expected) {
      ASSERT_FALSE(present);
    } else {
      ASSERT_TRUE(present);
      std::string str((const char *)value, length);
      ASSERT_EQ(str, *expected);
    }
    fdb_future_destroy(f);
    fdb_transaction_destroy(tr);
  }
}

}  // namespace hf3fs::kv
