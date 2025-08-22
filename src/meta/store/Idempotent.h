#pragma once

#include <folly/logging/xlog.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/kv/ITransaction.h"
#include "common/kv/KeyPrefix.h"
#include "common/serde/MessagePacket.h"
#include "common/serde/Serde.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/Nameof.hpp"
#include "common/utils/Result.h"
#include "common/utils/SerDeser.h"
#include "common/utils/UtcTime.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Service.h"

namespace hf3fs::meta::server {
/** Store transaction result to ensure idempotency during retries. Currently used for remove operations.
 */
struct Idempotent {
  static constexpr auto keyPrefix = kv::KeyPrefix::MetaIdempotent;

  template <typename T>
  struct Record {
    Record() requires(std::is_same_v<T, Void>) = default;
    explicit Record(const T &result)
        : result(result) {}

    SERDE_STRUCT_FIELD(clientId, Uuid::zero());
    SERDE_STRUCT_FIELD(requestId, Uuid::zero());
    SERDE_STRUCT_FIELD(timestamp, UtcTime());
    SERDE_STRUCT_FIELD(result, serde::Payload<T>());

   public:
    std::string packKey() const {
      // requestId + clientId to avoid hotspot
      XLOGF_IF(FATAL, clientId == Uuid::zero() || requestId == Uuid::zero(), "invalid uuid");
      return Serializer::serRawArgs(keyPrefix, requestId, clientId);
    }
  };

  template <class T, class ReqInfo>
  static CoTryTask<std::optional<Result<T>>> load(kv::IReadWriteTransaction &txn,
                                                  const Uuid clientId,
                                                  const Uuid requestId,
                                                  const ReqInfo &req) {
    if (clientId == Uuid::zero() || requestId == Uuid::zero()) {
      XLOGF(CRITICAL, "Request invalid uuid {} {}", clientId, requestId);
      co_return makeError(StatusCode::kInvalidArg, "Invalid uuid");
    }

    Record<Void> record;
    record.clientId = clientId;
    record.requestId = requestId;
    auto res = co_await txn.get(record.packKey());
    CO_RETURN_ON_ERROR(res);
    if (!res->has_value()) {
      co_return std::nullopt;
    }
    auto desRes = serde::deserialize(record, res->value());
    if (!desRes) {
      XLOGF(DFATAL, "IdempotentRecord deserialize failed, request {}, error {}", req, desRes.error());
      co_return makeError(StatusCode::kDataCorruption, "IdempotentRecord des failed");
    }
    if (record.clientId != clientId || record.requestId != requestId) {
      XLOGF(DFATAL, "IdempotentRecord mismatch, request {}, record {}", req, record);
      co_return makeError(MetaCode::kInconsistent, "IdempotentRecord uuid mismatch");
    }

    Result<T> result = makeError(StatusCode::kUnknown);
    auto desResult = serde::deserialize(result, record.result);
    if (!desResult) {
      XLOGF(DFATAL, "IdempotentRecord deserialize result failed, request {}, error {}", req, desResult.error());
      co_return makeError(StatusCode::kDataCorruption, "IdempotentRecord deserialize result failed");
    }
    XLOGF(CRITICAL, "Duplicated request {}, result {}, prev {}, now {}", req, result, record.timestamp, UtcTime::now());
    co_return std::optional(result);
  }

  template <class T>
  static CoTryTask<Void> store(kv::IReadWriteTransaction &txn,
                               const Uuid clientId,
                               const Uuid requestId,
                               const Result<T> &result) {
    Record<Result<T>> record(result);
    record.clientId = clientId;
    record.requestId = requestId;
    record.timestamp = UtcClock::now();

    auto key = record.packKey();
    auto value = serde::serialize(record);
    co_return co_await txn.set(key, value);
  }

  static CoTryTask<std::pair<std::string, bool>> clean(kv::IReadWriteTransaction &txn,
                                                       std::optional<std::string> prev,
                                                       Duration expire,
                                                       size_t limit,
                                                       size_t &total,
                                                       size_t &cleaned) {
    auto now = UtcClock::now();
    auto prefix = Serializer::serRawArgs(keyPrefix);
    auto begin = prev.value_or(prefix);
    XLOGF_IF(FATAL, begin < prefix, "{} < {}", begin, prefix);
    auto end = kv::TransactionHelper::prefixListEndKey(prefix);
    kv::IReadOnlyTransaction::KeySelector selBegin{begin, false};
    kv::IReadOnlyTransaction::KeySelector selEnd{end, false};
    auto res = co_await txn.getRange(selBegin, selEnd, limit);
    CO_RETURN_ON_ERROR(res);

    total = res->kvs.size();
    cleaned = 0;
    for (const auto &kv : res->kvs) {
      Record<Void> record;
      auto des = serde::deserialize(record, kv.value);
      if (!des) {
        XLOGF(CRITICAL, "IdempotentRecord deserialize failed {}", des.error());
        continue;
      }
      if (record.timestamp + expire < now) {
        cleaned++;
        CO_RETURN_ON_ERROR(co_await txn.clear(kv.key));
      }
    }

    auto nextPrev = res->kvs.empty() ? begin : res->kvs.back().key;
    co_return std::pair<std::string, bool>{nextPrev, res->hasMore};
  }
};

}  // namespace hf3fs::meta::server