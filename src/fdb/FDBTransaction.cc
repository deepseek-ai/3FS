#include "FDBTransaction.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/Random.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/futures/detail/Types.h>
#include <folly/lang/Bits.h>
#include <folly/logging/xlog.h>
#include <foundationdb/fdb_c_types.h>
#include <string>
#include <string_view>
#include <vector>

#include "common/monitor/Recorder.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/RandomUtils.h"
#include "common/utils/Result.h"
#include "common/utils/SerDeser.h"
#include "common/utils/Status.h"
#include "common/utils/UtcTime.h"
#include "fdb/FDB.h"
#include "foundationdb/fdb_c_options.g.h"

#define CHECK_FDB_ERRCODE(op, code)                          \
  do {                                                       \
    fdb_error_t errcode = code;                              \
    if (UNLIKELY(errcode != 0)) {                            \
      errcode_ = errcode;                                    \
      if (UNLIKELY(errcode == error_code_not_committed)) {   \
        XLOGF(WARN,                                          \
              "FDBTransaction {} failed, err: {}, msg: {}.", \
              magic_enum::enum_name(op),                     \
              errcode,                                       \
              fdb::DB::errorMsg(errcode));                   \
      } else {                                               \
        XLOGF(ERR,                                           \
              "FDBTransaction {} failed, err: {}, msg: {}.", \
              magic_enum::enum_name(op),                     \
              errcode,                                       \
              fdb::DB::errorMsg(errcode));                   \
      }                                                      \
      co_return makeFDBError(errcode, op);                   \
    }                                                        \
  } while (0)

#define FAULT_INJECTION_ON_COMMIT(db_name, injectMaybeCommitted)                                             \
  do {                                                                                                       \
    if (FAULT_INJECTION()) {                                                                                 \
      auto delayMs = folly::Random::rand32(20, 100);                                                         \
      errcode_ = hf3fs::RandomUtils::randomSelect(std::vector<fdb_error_t>{error_code_not_committed,         \
                                                                           error_code_commit_unknown_result, \
                                                                           error_code_transaction_too_old,   \
                                                                           error_code_tag_throttled});       \
      XLOGF(WARN, "Inject fault on commit, errCode: {}, delayMs: {}.", errcode_.load(), delayMs);            \
      if (errcode_ == error_code_commit_unknown_result && folly::Random::oneIn(2)) {                         \
        injectMaybeCommitted = true;                                                                         \
        XLOGF(WARN, "Inject maybeCommitted on commit, commit transaction in " #db_name ".");                 \
      } else {                                                                                               \
        co_return makeError(convertError(errcode_, op), "Inject fault on commit.");                          \
      }                                                                                                      \
    }                                                                                                        \
  } while (0)

#define FAULT_INJECTION_ON_GET(op_name)                                                                 \
  do {                                                                                                  \
    if (FAULT_INJECTION()) {                                                                            \
      auto delayMs = folly::Random::rand32(20, 100);                                                    \
      errcode_ = hf3fs::RandomUtils::randomSelect(                                                      \
          std::vector<fdb_error_t>{error_code_transaction_too_old, error_code_tag_throttled});          \
      XLOGF(WARN, "Inject fault on " #op_name ", errCode: {}, delayMs: {}.", errcode_.load(), delayMs); \
      co_await folly::coro::sleep(std::chrono::milliseconds(delayMs));                                  \
      co_return makeError(convertError(errcode_, op), "Inject fault on " #op_name);                     \
    }                                                                                                   \
  } while (0)

namespace hf3fs::kv {
namespace {

enum class Op {
  Get = 0,
  SnapshotGet,
  GetRange,
  SnapshotGetRange,
  AddReadConflict,
  Commit,
  Cancel,
  Set,
  SetVersionstampedKey,
  SetVersionstampedValue,
  Clear,
  ClearRange,
  GetReadVersion,
};

template <Op op>
struct OpRecorder {
  static constexpr bool recordFailed = false;
  static constexpr bool recordLatency = false;
  static monitor::CountRecorder totalRecorder;
};

#define OP_RECORDER_BASIC(op, name)                                              \
  template <>                                                                    \
  struct OpRecorder<op> {                                                        \
    static constexpr bool recordFailed = false;                                  \
    static constexpr bool recordLatency = false;                                 \
    static inline monitor::CountRecorder totalRecorder{"fdb_total_count_" name}; \
  }

#define OP_RECORDER_FAILED(op, name)                                               \
  template <>                                                                      \
  struct OpRecorder<op> {                                                          \
    static constexpr bool recordFailed = true;                                     \
    static constexpr bool recordLatency = false;                                   \
    static inline monitor::CountRecorder totalRecorder{"fdb_total_count_" name};   \
    static inline monitor::CountRecorder failedRecorder{"fdb_failed_count_" name}; \
  }

#define OP_RECORDER_LATENCY(op, name)                                              \
  template <>                                                                      \
  struct OpRecorder<op> {                                                          \
    static constexpr bool recordFailed = true;                                     \
    static constexpr bool recordLatency = true;                                    \
    static inline monitor::CountRecorder totalRecorder{"fdb_total_count_" name};   \
    static inline monitor::CountRecorder failedRecorder{"fdb_failed_count_" name}; \
    static inline monitor::LatencyRecorder latencyRecorder{"fdb_latency_" name};   \
  }

OP_RECORDER_BASIC(Op::Set, "set");
OP_RECORDER_BASIC(Op::SetVersionstampedKey, "set_versionstamped_key");
OP_RECORDER_BASIC(Op::SetVersionstampedValue, "set_versionstamped_value");
OP_RECORDER_BASIC(Op::Cancel, "cancel");
OP_RECORDER_BASIC(Op::Clear, "clear");
OP_RECORDER_BASIC(Op::ClearRange, "clear_range");
OP_RECORDER_FAILED(Op::AddReadConflict, "add_read_conflict");
OP_RECORDER_LATENCY(Op::Get, "get");
OP_RECORDER_LATENCY(Op::SnapshotGet, "snapshot_get");
OP_RECORDER_LATENCY(Op::GetRange, "get_range");
OP_RECORDER_LATENCY(Op::SnapshotGetRange, "snapshot_get_range");
OP_RECORDER_LATENCY(Op::Commit, "commit");
OP_RECORDER_LATENCY(Op::GetReadVersion, "get_read_version");

monitor::CountRecorder retryConflict("fdb.retry_conflict");
monitor::CountRecorder retryOther("fdb.retry_other");
monitor::LatencyRecorder retryBackoff("fdb.retry_backoff");

#define ERROR(name, code, desp) constexpr int error_code_##name = code;
FOLLY_PUSH_WARNING
FOLLY_GNU_DISABLE_WARNING("-Wpedantic")
#include "error_definitions.h"
FOLLY_POP_WARNING
#undef ERROR

uint32_t convertError(fdb_error_t code, Op op) {
  // see: Transaction::onError (foundationdb/fdbclient/NativeAPI.actor.cpp:7036)
  // see: fdb_error_predicate (foundationdb/bindings/c/fdb_c.cpp:71)
  assert(code != 0);
  switch (code) {
    case error_code_not_committed:
      return TransactionCode::kConflict;

    case error_code_commit_unknown_result:
      return TransactionCode::kMaybeCommitted;

    case error_code_proxy_memory_limit_exceeded:
      return TransactionCode::kResourceConstrained;

    case error_code_process_behind:
      return TransactionCode::kProcessBehind;

    case error_code_batch_transaction_throttled:
    case error_code_tag_throttled:
      return TransactionCode::kThrottled;

    case error_code_transaction_too_old:
      return TransactionCode::kTooOld;

    case error_code_future_version:
      return TransactionCode::kFutureVersion;

    case error_code_connection_failed:
    case error_code_blocked_from_network_thread:
      return TransactionCode::kNetworkError;

    case error_code_transaction_cancelled:
      return TransactionCode::kCanceled;

    case error_code_database_locked:
    case error_code_unknown_tenant:
      return TransactionCode::kRetryable;

    case error_code_cluster_version_changed:
      return op == Op::Commit ? TransactionCode::kMaybeCommitted : TransactionCode::kRetryable;
  }
  return TransactionCode::kFailed;
}

inline folly::Unexpected<Status> makeFDBError(fdb_error_t code, Op op) {
  auto msg = fmt::format("FDB error: {}, msg: {}", code, fdb::DB::errorMsg(code));
  return makeError(convertError(code, op), std::move(msg));
}

template <Op op>
struct OpWrapper {
  using Clock = std::chrono::steady_clock;

  template <typename F>
  static std::invoke_result_t<F, Op> run(F &&f) {
    OpRecorder<op>::totalRecorder.addSample(1);
    auto start = Clock::now();
    auto result = co_await f(op);
    auto duration = Clock::now() - start;
    if (UNLIKELY(result.hasError())) {
      if constexpr (OpRecorder<op>::recordFailed) {
        auto tagset = monitor::TagSet::create("statusCode", String(StatusCode::toString(result.error().code())));
        OpRecorder<op>::failedRecorder.addSample(1, tagset);
      }
    }
    if constexpr (OpRecorder<op>::recordLatency) {
      OpRecorder<op>::latencyRecorder.addSample(duration);
    }
    co_return result;
  }
};

std::string appendOffset(std::string_view str, uint32_t offset) {
  String buf;
  buf.reserve(str.size() + sizeof(offset));
  Serializer ser{buf};
  ser.putRaw(str.data(), str.size());
  ser.put(folly::Endian::little32(offset));
  assert(buf.size() == str.size() + 4);
  return buf;
}

}  // namespace

CoTryTask<std::optional<String>> FDBTransaction::get(std::string_view key) {
  co_return co_await OpWrapper<Op::Get>::run([&](Op op) -> CoTryTask<std::optional<String>> {
    FAULT_INJECTION_ON_GET(get);

    auto result = co_await tr_.get(key);
    CHECK_FDB_ERRCODE(op, result.error());
    co_return std::move(result.value());
  });
}

CoTryTask<FDBTransaction::GetRangeResult> FDBTransaction::getRange(const KeySelector &begin,
                                                                   const KeySelector &end,
                                                                   int32_t limit) {
  co_return co_await OpWrapper<Op::GetRange>::run([&](Op op) -> CoTryTask<GetRangeResult> {
    FAULT_INJECTION_ON_GET(getRange);

    fdb::KeySelector innerBegin(begin.key, !begin.inclusive, 1);
    fdb::KeySelector innerEnd(end.key, end.inclusive, 1);
    fdb::GetRangeLimits limits(limit);
    auto result = co_await tr_.getRange(innerBegin, innerEnd, limits, /* iteration */ 0, /* snapshot */ false);
    CHECK_FDB_ERRCODE(op, result.error());
    co_return GetRangeResult(std::move(result.value().first), result.value().second);
  });
}

CoTryTask<FDBTransaction::GetRangeResult> FDBTransaction::snapshotGetRange(const KeySelector &begin,
                                                                           const KeySelector &end,
                                                                           int32_t limit) {
  co_return co_await OpWrapper<Op::SnapshotGetRange>::run([&](Op op) -> CoTryTask<GetRangeResult> {
    FAULT_INJECTION_ON_GET(snapshotGetRange);

    fdb::KeySelector innerBegin(begin.key, !begin.inclusive, 1);
    fdb::KeySelector innerEnd(end.key, end.inclusive, 1);
    fdb::GetRangeLimits limits(limit);
    auto result = co_await tr_.getRange(innerBegin, innerEnd, limits, /* iteration */ 0, /* snapshot */ true);
    CHECK_FDB_ERRCODE(op, result.error());
    co_return GetRangeResult(std::move(result.value().first), result.value().second);
  });
}

CoTryTask<void> FDBTransaction::cancel() {
  co_return co_await OpWrapper<Op::Cancel>::run([&](Op) -> CoTryTask<void> {
    tr_.cancel();
    co_return Void{};
  });
}

CoTryTask<std::optional<String>> FDBTransaction::snapshotGet(std::string_view key) {
  co_return co_await OpWrapper<Op::SnapshotGet>::run([&](Op op) -> CoTryTask<std::optional<String>> {
    FAULT_INJECTION_ON_GET(snapshotGet);

    auto result = co_await tr_.get(key, /* snapshot = */ true);
    CHECK_FDB_ERRCODE(op, result.error());
    co_return std::move(result.value());
  });
}

CoTryTask<void> FDBTransaction::addReadConflict(std::string_view key) {
  co_return co_await OpWrapper<Op::AddReadConflict>::run([&](Op op) -> CoTryTask<void> {
    String endKey = TransactionHelper::keyAfter(key);
    fdb::KeyRangeView range;
    range.beginKey = key;
    range.endKey = endKey;
    auto result = tr_.addConflictRange(range, FDBConflictRangeType::FDB_CONFLICT_RANGE_TYPE_READ);
    CHECK_FDB_ERRCODE(op, result);
    co_return Void{};
  });
}

CoTryTask<void> FDBTransaction::addReadConflictRange(std::string_view begin, std::string_view end) {
  co_return co_await OpWrapper<Op::AddReadConflict>::run([&](Op op) -> CoTryTask<void> {
    fdb::KeyRangeView range;
    range.beginKey = begin;
    range.endKey = end;
    auto result = tr_.addConflictRange(range, FDBConflictRangeType::FDB_CONFLICT_RANGE_TYPE_READ);
    CHECK_FDB_ERRCODE(op, result);
    co_return Void{};
  });
}

CoTryTask<void> FDBTransaction::set(std::string_view key, std::string_view value) {
  co_return co_await OpWrapper<Op::Set>::run([&](Op) -> CoTryTask<void> {
    tr_.set(key, value);
    co_return Void{};
  });
}

CoTryTask<void> FDBTransaction::setVersionstampedKey(std::string_view key, uint32_t offset, std::string_view value) {
  if (offset + sizeof(kv::Versionstamp) > key.size()) {
    co_return makeError(
        StatusCode::kInvalidArg,
        fmt::format("setVersionstampedKey: {} + sizeof(kv::Versionstamp) > key.size {}", offset, key.size()));
  }
  co_return co_await OpWrapper<Op::SetVersionstampedKey>::run([&](Op) -> CoTryTask<void> {
    tr_.atomicOp(appendOffset(key, offset), value, FDB_MUTATION_TYPE_SET_VERSIONSTAMPED_KEY);
    co_return Void{};
  });
}

CoTryTask<void> FDBTransaction::setVersionstampedValue(std::string_view key, std::string_view value, uint32_t offset) {
  if (offset + sizeof(kv::Versionstamp) > value.size()) {
    co_return makeError(
        StatusCode::kInvalidArg,
        fmt::format("setVersionstampedValue: {} + sizeof(kv::Versionstamp) > value.size {}", offset, value.size()));
  }
  co_return co_await OpWrapper<Op::SetVersionstampedValue>::run([&](Op) -> CoTryTask<void> {
    tr_.atomicOp(key, appendOffset(value, offset), FDB_MUTATION_TYPE_SET_VERSIONSTAMPED_VALUE);
    co_return Void{};
  });
}

CoTryTask<void> FDBTransaction::clear(std::string_view key) {
  co_return co_await OpWrapper<Op::Clear>::run([&](Op) -> CoTryTask<void> {
    tr_.clear(key);
    co_return Void{};
  });
}

CoTryTask<void> FDBTransaction::clearRange(std::string_view begin, std::string_view end) {
  co_return co_await OpWrapper<Op::ClearRange>::run([&](Op) -> CoTryTask<void> {
    fdb::KeyRangeView range;
    range.beginKey = begin;
    range.endKey = end;
    tr_.clearRange(range);
    co_return Void{};
  });
}

CoTryTask<void> FDBTransaction::commit() {
  co_return co_await OpWrapper<Op::Commit>::run([&](Op op) -> CoTryTask<void> {
    bool injectMaybeCommitted = false;
    FAULT_INJECTION_ON_COMMIT(FoundationDB, injectMaybeCommitted);

    auto result = co_await tr_.commit();
    if (UNLIKELY(injectMaybeCommitted)) {
      errcode_ = error_code_commit_unknown_result;
      XLOGF(WARN, "Inject maybeCommitted error after commit, FoundationDB commit result is {}.", result.error());
      co_return makeError(convertError(errcode_, op), "Fault injection commit unknown result.");
    }
    CHECK_FDB_ERRCODE(op, result.error());
    co_return Void{};
  });
}

void FDBTransaction::reset() {
  errcode_ = 0;
  tr_.reset();
}

Result<Void> FDBTransaction::setOption(FDBTransactionOption option, std::string_view value) {
  fdb_error_t errcode = tr_.setOption(option, value);
  if (errcode != 0) {
    XLOGF(ERR, "FDBTransaction failed to set option {}, errcode {}", magic_enum::enum_name(option), errcode);
    return makeError(StatusCode::kInvalidArg);
  }
  return Void{};
}

CoTask<bool> FDBTransaction::onError(fdb_error_t errcode) {
  if (errcode == error_code_not_committed) {
    retryConflict.addSample(1);
  } else {
    retryOther.addSample(1);
  }
  auto begin = SteadyClock::now();
  auto ret = co_await tr_.onError(errcode);
  retryBackoff.addSample(SteadyClock::now() - begin);
  co_return ret.error() == 0;
}

Status FDBTransaction::testFDBError(fdb_error_t errCode, bool commit) {
  return makeFDBError(errCode, commit ? Op::Commit : Op::Get).error();
}

CoTryTask<int64_t> FDBTransaction::getReadVersion() {
  co_return co_await OpWrapper<Op::GetReadVersion>::run([&](Op op) -> CoTryTask<int64_t> {
    auto r = co_await tr_.getReadVersion();
    CHECK_FDB_ERRCODE(Op::GetReadVersion, r.error());
    co_return r.value();
  });
}

void FDBTransaction::setReadVersion(int64_t version) {
  if (version >= 0) {
    tr_.setReadVersion(version);
  }
}

int64_t FDBTransaction::getCommittedVersion() {
  int64_t version;
  auto errcode = tr_.getCommittedVersion(&version);
  return errcode == 0 ? version : -1;
}

}  // namespace hf3fs::kv
