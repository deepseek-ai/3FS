#pragma once

#include <folly/Utility.h>
#include <folly/experimental/coro/FutureUtil.h>
#include <folly/experimental/coro/Promise.h>
#include <folly/experimental/coro/Task.h>

#include "common/kv/ITransaction.h"
#include "common/utils/String.h"
#include "foundationdb/fdb_c_options.g.h"
#include "foundationdb/fdb_c_types.h"

#define FDB_API_VERSION 710
#include <foundationdb/fdb_c.h>

namespace hf3fs::kv {

class FDBKVEngine;

namespace fdb {

using folly::coro::Task;
using KeyValue = IReadOnlyTransaction::KeyValue;

// Structs for returned values.
struct KeyRange {
  String beginKey;
  String endKey;
};

// Structs for parameters.
struct KeyRangeView {
  std::string_view beginKey;
  std::string_view endKey;
};

struct KeySelector {
  std::string_view key;  // Find the last item less than key
  bool orEqual = true;   // (or equal to key, if this is true)
  int offset = 0;        // and then move forward this many items (or backward if negative)

  KeySelector() = default;
  KeySelector(std::string_view key, bool orEqual, int offset)
      : key(std::move(key)),
        orEqual(orEqual),
        offset(offset) {}
};

struct GetRangeLimits {
  std::optional<uint32_t> rows;
  std::optional<uint32_t> bytes;

  explicit GetRangeLimits(std::optional<uint32_t> r = {}, std::optional<uint32_t> b = {})
      : rows(r),
        bytes(b) {}
};

template <class T, class V>
class Result {
 public:
  fdb_error_t error() const { return error_; };
  V &value() { return value_; }
  const V &value() const { return value_; }

 protected:
  friend class DB;
  friend class Transaction;

  static Task<T> toTask(FDBFuture *f);
  void extractValue();

 protected:
  struct FDBFutureDeleter {
    constexpr FDBFutureDeleter() noexcept = default;
    void operator()(FDBFuture *future) const { future ? fdb_future_destroy(future) : void(); }
  };
  std::unique_ptr<FDBFuture, FDBFutureDeleter> future_;
  fdb_error_t error_ = 0;
  V value_{};
};

class Int64Result : public Result<Int64Result, int64_t> {};
class KeyResult : public Result<KeyResult, String> {};
class ValueResult : public Result<ValueResult, std::optional<String>> {};
class KeyArrayResult : public Result<KeyArrayResult, std::vector<String>> {};
class StringArrayResult : public Result<StringArrayResult, std::vector<String>> {};
class KeyValueArrayResult : public Result<KeyValueArrayResult, std::pair<std::vector<KeyValue>, bool>> {};
class KeyRangeArrayResult : public Result<KeyRangeArrayResult, std::vector<KeyRange>> {};
struct EmptyValue {};
class EmptyResult : public Result<EmptyResult, EmptyValue> {};

class DB {
 public:
  static fdb_error_t selectAPIVersion(int version);
  static std::string_view errorMsg(fdb_error_t code);
  static bool evaluatePredicate(int predicate_test, fdb_error_t code);

  // network
  static fdb_error_t setNetworkOption(FDBNetworkOption option, std::string_view value = {});
  static fdb_error_t setupNetwork();
  static fdb_error_t runNetwork();
  static fdb_error_t stopNetwork();

 public:
  DB() = default;
  explicit DB(const String &clusterFilePath, bool readonly)
      : readonly_(readonly) {
    auto path = clusterFilePath.empty() ? nullptr : clusterFilePath.c_str();
    FDBDatabase *db;
    error_ = fdb_create_database(path, &db);
    if (error_ == 0) {
      db_.reset(db);
    }
  }

  fdb_error_t error() const { return error_; }
  explicit operator bool() const { return error() == 0; }
  operator FDBDatabase *() const { return db_.get(); }

  fdb_error_t setOption(FDBDatabaseOption option, std::string_view value = {});

  Task<Int64Result> rebootWorker(std::string_view address, bool check = false, int duration = 0);
  Task<EmptyResult> forceRecoveryWithDataLoss(std::string_view dcid);
  Task<EmptyResult> createSnapshot(std::string_view uid, std::string_view snapCommand);
  Task<KeyResult> purgeBlobGranules(const KeyRangeView &range, int64_t purgeVersion, fdb_bool_t force);
  Task<EmptyResult> waitPurgeGranulesComplete(std::string_view purgeKey);

  bool readonly() const { return readonly_; }

 private:
  friend class hf3fs::kv::FDBKVEngine;

  struct FDBDatabaseDeleter {
    constexpr FDBDatabaseDeleter() noexcept = default;
    void operator()(FDBDatabase *db) const { db ? fdb_database_destroy(db) : void(); }
  };
  std::unique_ptr<FDBDatabase, FDBDatabaseDeleter> db_;
  bool readonly_ = false;
  fdb_error_t error_ = 1;
};

class Transaction final {
 public:
  Transaction(DB &db)
      : readonly_(db.readonly()) {
    FDBTransaction *tr;
    error_ = fdb_database_create_transaction(db, &tr);
    if (error_ == 0) {
      tr_.reset(tr);
    }
  }

  fdb_error_t error() const { return error_; }
  explicit operator bool() const { return error_ == 0 && tr_; }

  fdb_error_t setOption(FDBTransactionOption option, std::string_view value = {});

  // Read transaction
  void setReadVersion(int64_t version);
  Task<Int64Result> getReadVersion();

  Task<ValueResult> get(std::string_view key, fdb_bool_t snapshot = false);
  Task<KeyResult> getKey(const KeySelector &selector, fdb_bool_t snapshot = false);
  Task<EmptyResult> watch(std::string_view key);

  Task<StringArrayResult> getAddressesForKey(std::string_view key);
  Task<KeyValueArrayResult> getRange(const KeySelector &begin,
                                     const KeySelector &end,
                                     GetRangeLimits limits = GetRangeLimits(),
                                     int iteration = 0,
                                     bool snapshot = false,
                                     bool reverse = false,
                                     FDBStreamingMode streamingMode = FDB_STREAMING_MODE_SERIAL);

  Task<Int64Result> getEstimatedRangeSizeBytes(const KeyRangeView &range);
  Task<KeyArrayResult> getRangeSplitPoints(const KeyRangeView &range, int64_t chunkSize);

  Task<EmptyResult> onError(fdb_error_t err);

  void cancel();
  void reset();

  // Write transaction
  fdb_error_t addConflictRange(const KeyRangeView &range, FDBConflictRangeType type);

  void atomicOp(std::string_view key, std::string_view param, FDBMutationType operationType);
  void set(std::string_view key, std::string_view value);
  void clear(std::string_view key);
  void clearRange(const KeyRangeView &range);

  Task<EmptyResult> commit();
  fdb_error_t getCommittedVersion(int64_t *outVersion);
  Task<Int64Result> getApproximateSize();
  Task<KeyResult> getVersionstamp();

 private:
  struct FDBTransactionDeleter {
    constexpr FDBTransactionDeleter() noexcept = default;
    void operator()(FDBTransaction *tr) const { tr ? fdb_transaction_destroy(tr) : void(); }
  };
  std::unique_ptr<FDBTransaction, FDBTransactionDeleter> tr_;
  bool readonly_ = false;
  fdb_error_t error_ = 1;
};
}  // namespace fdb
}  // namespace hf3fs::kv
