#include "FDB.h"

#include <atomic>
#include <folly/CancellationToken.h>
#include <folly/Likely.h>
#include <folly/experimental/coro/Baton.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/DetachOnCancel.h>
#include <folly/logging/xlog.h>

#include "foundationdb/fdb_c_types.h"

namespace hf3fs::kv::fdb {

#define TOU8(str) reinterpret_cast<const uint8_t *>(str.data()), str.length()
#define VIEW(name) std::string_view(reinterpret_cast<const char *>(name), name##_length)
#define SELECTOR(selector) TOU8(selector.key), selector.orEqual, selector.offset

template <>
void Result<Int64Result, int64_t>::extractValue() {
  error_ = fdb_future_get_int64(future_.get(), &value_);
}

template <>
void Result<KeyResult, String>::extractValue() {
  const uint8_t *data;
  int data_length;
  error_ = fdb_future_get_key(future_.get(), &data, &data_length);
  if (error_ == 0) {
    value_ = VIEW(data);
  }
}

template <>
void Result<ValueResult, std::optional<String>>::extractValue() {
  fdb_bool_t present = false;
  const uint8_t *data;
  int data_length;
  error_ = fdb_future_get_value(future_.get(), &present, &data, &data_length);
  if (error_ == 0 && present) {
    value_ = VIEW(data);
  }
}

template <>
void Result<KeyArrayResult, std::vector<String>>::extractValue() {
  const FDBKey *keys;
  int count = 0;
  error_ = fdb_future_get_key_array(future_.get(), &keys, &count);
  if (error_ == 0 && count) {
    value_.reserve(count);
    for (int i = 0; i < count; ++i) {
      value_.emplace_back(VIEW(keys[i].key));
    }
  }
}

template <>
void Result<StringArrayResult, std::vector<String>>::extractValue() {
  const char **strings;
  int count = 0;
  error_ = fdb_future_get_string_array(future_.get(), &strings, &count);
  if (error_ == 0 && count) {
    value_.reserve(count);
    for (int i = 0; i < count; ++i) {
      value_.emplace_back(strings[i]);
    }
  }
}

template <>
void Result<KeyValueArrayResult, std::pair<std::vector<KeyValue>, bool>>::extractValue() {
  const FDBKeyValue *kv;
  int count;
  fdb_bool_t more = false;
  error_ = fdb_future_get_keyvalue_array(future_.get(), &kv, &count, &more);
  if (error_ == 0 && count) {
    auto &vec = value_.first;
    vec.reserve(count);
    for (int i = 0; i < count; ++i) {
      vec.emplace_back(VIEW(kv[i].key), VIEW(kv[i].value));
    }
    value_.second = more;
  }
}

template <>
void Result<KeyRangeArrayResult, std::vector<KeyRange>>::extractValue() {
  const FDBKeyRange *ranges;
  int count;
  error_ = fdb_future_get_keyrange_array(future_.get(), &ranges, &count);
  if (error_ == 0 && count) {
    value_.reserve(count);
    for (int i = 0; i < count; ++i) {
      KeyRange range;
      range.beginKey = VIEW(ranges[i].begin_key);
      range.endKey = VIEW(ranges[i].end_key);
      value_.push_back(std::move(range));
    }
  }
}

template <>
void Result<EmptyResult, EmptyValue>::extractValue() {}

static void coroCallback(FDBFuture *, void *para) {
  auto baton = static_cast<folly::coro::Baton *>(para);
  baton->post();
}

template <class T, class V>
Task<T> Result<T, V>::toTask(FDBFuture *f) {
  T result;
  result.future_.reset(f);

  folly::coro::Baton baton;
  result.error_ = fdb_future_set_callback(f, coroCallback, &baton);
  if (result.error()) {
    co_return result;
  }
  std::atomic_bool cancel = false;
  auto token = co_await folly::coro::co_current_cancellation_token;
  folly::CancellationCallback cb(token, [&]() {
    cancel = true;
    fdb_future_cancel(f);
  });
  co_await baton;
  if (cancel.load()) {
    throw folly::OperationCancelled();
  }

  result.error_ = fdb_future_get_error(f);
  if (result.error()) {
    co_return result;
  }

  result.extractValue();
  co_return result;
}

// Global
fdb_error_t DB::selectAPIVersion(int version) { return fdb_select_api_version(version); }
std::string_view DB::errorMsg(fdb_error_t code) { return fdb_get_error(code); }
bool DB::evaluatePredicate(int predicate_test, fdb_error_t code) { return fdb_error_predicate(predicate_test, code); }

// Network
fdb_error_t DB::setNetworkOption(FDBNetworkOption option, std::string_view value /* = {} */) {
  return fdb_network_set_option(option, TOU8(value));
}
fdb_error_t DB::setupNetwork() { return fdb_setup_network(); }
fdb_error_t DB::runNetwork() { return fdb_run_network(); }
fdb_error_t DB::stopNetwork() { return fdb_stop_network(); }

// DB
fdb_error_t DB::setOption(FDBDatabaseOption option, std::string_view value) {
  return fdb_database_set_option(db_.get(), option, TOU8(value));
}

Task<Int64Result> DB::rebootWorker(std::string_view address, bool check /* = false */, int duration /* = 0 */) {
  co_return co_await Int64Result::toTask(fdb_database_reboot_worker(db_.get(), TOU8(address), check, duration));
}

Task<EmptyResult> DB::forceRecoveryWithDataLoss(std::string_view dcid) {
  co_return co_await EmptyResult::toTask(fdb_database_force_recovery_with_data_loss(db_.get(), TOU8(dcid)));
}

Task<EmptyResult> DB::createSnapshot(std::string_view uid, std::string_view snapCommand) {
  co_return co_await EmptyResult::toTask(fdb_database_create_snapshot(db_.get(), TOU8(uid), TOU8(snapCommand)));
}

Task<KeyResult> DB::purgeBlobGranules(const KeyRangeView &range, int64_t purgeVersion, fdb_bool_t force) {
  co_return co_await KeyResult::toTask(
      fdb_database_purge_blob_granules(db_.get(), TOU8(range.beginKey), TOU8(range.endKey), purgeVersion, force));
}

Task<EmptyResult> DB::waitPurgeGranulesComplete(std::string_view purgeKey) {
  co_return co_await EmptyResult::toTask(fdb_database_wait_purge_granules_complete(db_.get(), TOU8(purgeKey)));
}

void Transaction::reset() { fdb_transaction_reset(tr_.get()); }

void Transaction::cancel() { fdb_transaction_cancel(tr_.get()); }

[[nodiscard]] fdb_error_t Transaction::setOption(FDBTransactionOption option, std::string_view value /* = {} */) {
  return fdb_transaction_set_option(tr_.get(), option, TOU8(value));
}

void Transaction::setReadVersion(int64_t version) { fdb_transaction_set_read_version(tr_.get(), version); }

Task<Int64Result> Transaction::getReadVersion() {
  co_return co_await Int64Result::toTask(fdb_transaction_get_read_version(tr_.get()));
}

Task<Int64Result> Transaction::getApproximateSize() {
  co_return co_await Int64Result::toTask(fdb_transaction_get_approximate_size(tr_.get()));
}

Task<KeyResult> Transaction::getVersionstamp() {
  co_return co_await KeyResult::toTask(fdb_transaction_get_versionstamp(tr_.get()));
}

Task<ValueResult> Transaction::get(std::string_view key, fdb_bool_t snapshot /* = false */) {
  co_return co_await ValueResult::toTask(fdb_transaction_get(tr_.get(), TOU8(key), snapshot));
}

Task<KeyResult> Transaction::getKey(const KeySelector &selector, fdb_bool_t snapshot /* = false */) {
  co_return co_await KeyResult::toTask(fdb_transaction_get_key(tr_.get(), SELECTOR(selector), snapshot));
}

Task<StringArrayResult> Transaction::getAddressesForKey(std::string_view key) {
  co_return co_await StringArrayResult::toTask(fdb_transaction_get_addresses_for_key(tr_.get(), TOU8(key)));
}

Task<KeyValueArrayResult> Transaction::getRange(const KeySelector &begin,
                                                const KeySelector &end,
                                                GetRangeLimits limits /* = GetRangeLimits() */,
                                                int iteration /* = 0 */,
                                                bool snapshot /* = false */,
                                                bool reverse /* = false */,
                                                FDBStreamingMode streamingMode /* = FDB_STREAMING_MODE_SERIAL */) {
  co_return co_await KeyValueArrayResult::toTask(fdb_transaction_get_range(tr_.get(),
                                                                           SELECTOR(begin),
                                                                           SELECTOR(end),
                                                                           limits.rows.value_or(-1),
                                                                           limits.bytes.value_or(-1),
                                                                           streamingMode,
                                                                           iteration,
                                                                           snapshot,
                                                                           reverse));
}

Task<Int64Result> Transaction::getEstimatedRangeSizeBytes(const KeyRangeView &range) {
  co_return co_await Int64Result::toTask(
      fdb_transaction_get_estimated_range_size_bytes(tr_.get(), TOU8(range.beginKey), TOU8(range.endKey)));
}

Task<KeyArrayResult> Transaction::getRangeSplitPoints(const KeyRangeView &range, int64_t chunkSize) {
  co_return co_await KeyArrayResult::toTask(
      fdb_transaction_get_range_split_points(tr_.get(), TOU8(range.beginKey), TOU8(range.endKey), chunkSize));
}

Task<EmptyResult> Transaction::watch(std::string_view key) {
  co_return co_await EmptyResult::toTask(fdb_transaction_watch(tr_.get(), TOU8(key)));
}

Task<EmptyResult> Transaction::commit() {
  if (UNLIKELY(readonly_)) {
    // Prevent tools like admin_cli or fsck from mistakenly modifying data
    XLOGF(CRITICAL, "disallow call commit on a read-only FDBContext!!!");
    EmptyResult result;
    result.error_ = 1000; /* operation failed */
    co_return result;
  }
  co_return co_await EmptyResult::toTask(fdb_transaction_commit(tr_.get()));
}

Task<EmptyResult> Transaction::onError(fdb_error_t err) {
  co_return co_await EmptyResult::toTask(fdb_transaction_on_error(tr_.get(), err));
}

void Transaction::clear(std::string_view key) { return fdb_transaction_clear(tr_.get(), TOU8(key)); }

void Transaction::clearRange(const KeyRangeView &range) {
  fdb_transaction_clear_range(tr_.get(), TOU8(range.beginKey), TOU8(range.endKey));
}

void Transaction::set(std::string_view key, std::string_view value) {
  fdb_transaction_set(tr_.get(), TOU8(key), TOU8(value));
}

void Transaction::atomicOp(std::string_view key, std::string_view param, FDBMutationType operationType) {
  return fdb_transaction_atomic_op(tr_.get(), TOU8(key), TOU8(param), operationType);
}

[[nodiscard]] fdb_error_t Transaction::getCommittedVersion(int64_t *outVersion) {
  return fdb_transaction_get_committed_version(tr_.get(), outVersion);
}

fdb_error_t Transaction::addConflictRange(const KeyRangeView &range, FDBConflictRangeType type) {
  return fdb_transaction_add_conflict_range(tr_.get(), TOU8(range.beginKey), TOU8(range.endKey), type);
}

}  // namespace hf3fs::kv::fdb
