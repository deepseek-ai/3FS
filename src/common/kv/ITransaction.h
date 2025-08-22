#pragma once

#include <cstdint>
#include <folly/Utility.h>
#include <folly/logging/xlog.h>
#include <optional>
#include <string_view>
#include <vector>

#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/Status.h"
#include "common/utils/String.h"
#include "common/utils/Transform.h"

namespace hf3fs::kv {

// - The metadata version key "\xff/metadataVersion" is a key intended to help layers deal with hot keys. The value of
// this key is sent to clients along with the read version from the proxy, so a client can read its value without
// communicating with a storage server. The value of this key can only be set with SetVersionStampValue operations.
// - fdbdr will always capture changes to this key, regardless of the specified range for the DR.
// - Restore will set this key right before a restore is complete.
// - Calling setVersion on a transaction does not include the value of the metadataVersionKey, so a cache of recent
// version to metadataVersion mappings are kept in the database context. This is useful when caching read versions from
// one transaction and calling setVersion on a different transaction to avoid waiting for read version there.
static constexpr std::string_view kMetadataVersionKey = "\xff/metadataVersion";

// A versionstamp is a 10 byte, unique, monotonically (but not sequentially) increasing value for each committed
// transaction. The first 8 bytes are the committed version of the database (serialized in big-endian order). The last
// 2 bytes are monotonic in the serialization order for transactions (serialized in big-endian order).
using Versionstamp = std::array<uint8_t, 10>;
static_assert(sizeof(Versionstamp) == 10);

class IReadOnlyTransaction {
 public:
  virtual ~IReadOnlyTransaction() = default;

  virtual void setReadVersion(int64_t version) = 0;

  virtual CoTryTask<std::optional<String>> snapshotGet(std::string_view key) = 0;

  virtual CoTryTask<std::optional<String>> get(std::string_view key) {
    // fallback to snapshotGet by default.
    co_return co_await snapshotGet(key);
  }

  struct KeyValue {
    String key;
    String value;

    KeyValue(String k, String v)
        : key(std::move(k)),
          value(std::move(v)) {}

    KeyValue(std::string_view k, std::string_view v)
        : key(k),
          value(v) {}

    std::tuple<const String &, const String &> pair() const { return {key, value}; }
  };

  struct KeySelector {
    std::string_view key;
    bool inclusive;

    KeySelector(std::string_view k, bool argInclusive)
        : key(k),
          inclusive(argInclusive) {}
  };

  struct GetRangeResult : public folly::MoveOnly {
    std::vector<KeyValue> kvs;
    bool hasMore;

    GetRangeResult(std::vector<KeyValue> argKvs, bool argHasMore)
        : kvs(std::move(argKvs)),
          hasMore(argHasMore) {}
  };
  virtual CoTryTask<GetRangeResult> snapshotGetRange(const KeySelector &begin,
                                                     const KeySelector &end,
                                                     int32_t limit) = 0;
  virtual CoTryTask<GetRangeResult> getRange(const KeySelector &begin, const KeySelector &end, int32_t limit) = 0;

  virtual CoTryTask<void> cancel() = 0;
  virtual void reset() = 0;
};

class IReadWriteTransaction : public IReadOnlyTransaction {
 public:
  ~IReadWriteTransaction() override = default;

  // The difference of `snapshotGet` and `get` is the former needs no conflict validation and hence won't cause a
  // read-write transaction fail.
  CoTryTask<std::optional<String>> get(std::string_view key) override = 0;
  CoTryTask<GetRangeResult> getRange(const KeySelector &begin, const KeySelector &end, int32_t limit) override = 0;

  // Add a read conflict key to transaction without performing associated read.
  virtual CoTryTask<void> addReadConflict(std::string_view key) = 0;
  virtual CoTryTask<void> addReadConflictRange(std::string_view begin, std::string_view end) = 0;

  virtual CoTryTask<void> set(std::string_view key, std::string_view value) = 0;
  virtual CoTryTask<void> clear(std::string_view key) = 0;

  // A versionstamp is a 10 byte, unique, monotonically (but not sequentially) increasing value for each committed
  // transaction. The first 8 bytes are the committed version of the database (serialized in big-endian order). The last
  // 2 bytes are monotonic in the serialization order for transactions (serialized in big-endian order).
  virtual CoTryTask<void> setVersionstampedKey(std::string_view key, uint32_t offset, std::string_view value) = 0;
  virtual CoTryTask<void> setVersionstampedValue(std::string_view key, std::string_view value, uint32_t offset) = 0;

  virtual CoTryTask<void> commit() = 0;
  virtual int64_t getCommittedVersion() = 0;
};

struct TransactionHelper {
  static bool isTransactionError(const Status &error);

  static bool isRetryable(const Status &error, bool allowMaybeCommitted);

  static String keyAfter(std::string_view key);

  static String prefixListEndKey(std::string_view prefix);

  struct ListByPrefixOptions {
    bool snapshot = true;
    bool inclusive = true;
    size_t limit = 0;

    ListByPrefixOptions() = default;
    ListByPrefixOptions &withSnapshot(bool v) {
      snapshot = v;
      return *this;
    }
    ListByPrefixOptions &withInclusive(bool v) {
      inclusive = v;
      return *this;
    }
    ListByPrefixOptions &withLimit(size_t v) {
      limit = v;
      return *this;
    }
  };

  using KeyValue = IReadOnlyTransaction::KeyValue;
  static CoTryTask<std::vector<KeyValue>> listByPrefix(IReadOnlyTransaction &txn,
                                                       std::string_view prefix,
                                                       ListByPrefixOptions options);

  template <typename T, typename Fn>
  static CoTryTask<std::vector<T>> listByPrefix(IReadOnlyTransaction &txn,
                                                std::string_view prefix,
                                                ListByPrefixOptions options,
                                                Fn &&unpack) {
    auto listRes = co_await listByPrefix(txn, prefix, options);
    CO_RETURN_ON_ERROR(listRes);
    std::vector<T> res;
    for (auto &[k, v] : *listRes) {
      auto entry = unpack(k, v);
      CO_RETURN_ON_ERROR(entry);
      res.push_back(std::move(*entry));
    }
    co_return res;
  }
};

}  // namespace hf3fs::kv
