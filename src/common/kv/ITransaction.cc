#include "ITransaction.h"

#include <folly/logging/xlog.h>

#include "common/utils/StringUtils.h"

namespace hf3fs::kv {
bool TransactionHelper::isTransactionError(const Status &error) {
  return StatusCode::typeOf(error.code()) == StatusCodeType::Transaction;
}

bool TransactionHelper::isRetryable(const Status &error, bool allowMaybeCommitted) {
  switch (error.code()) {
    case TransactionCode::kConflict:
    case TransactionCode::kThrottled:
    case TransactionCode::kTooOld:
    case TransactionCode::kRetryable:
    case TransactionCode::kResourceConstrained:
    case TransactionCode::kProcessBehind:
    case TransactionCode::kFutureVersion:
      return true;

    case TransactionCode::kMaybeCommitted:
      return allowMaybeCommitted;

    case TransactionCode::kNetworkError:
    case TransactionCode::kCanceled:
      return false;
  }
  return false;
}

String TransactionHelper::keyAfter(std::string_view key) {
  String after;
  after.reserve(key.size() + 1);
  after.append(key.begin(), key.size());
  after.push_back('\0');

  return after;
}

String TransactionHelper::prefixListEndKey(std::string_view prefix) {
  String endKey(prefix);
  while (!endKey.empty()) {
    auto &c = endKey.back();
    if (c != '\xff') {
      ++c;
      break;
    } else {
      endKey.pop_back();
    }
  }

  return endKey;
}

auto TransactionHelper::listByPrefix(IReadOnlyTransaction &txn, std::string_view prefix, ListByPrefixOptions options)
    -> CoTryTask<std::vector<KeyValue>> {
  assert(!prefix.empty() && prefix[0] != '\xff');
  auto loadFunc = options.snapshot ? &IReadOnlyTransaction::snapshotGetRange : &IReadOnlyTransaction::getRange;
  String beginKey(prefix);
  String endKey = prefixListEndKey(beginKey);
  IReadOnlyTransaction::KeySelector begin(beginKey, options.inclusive);
  IReadOnlyTransaction::KeySelector end(endKey, false);
  std::vector<KeyValue> res;
  auto limit = options.limit;
  while (limit == 0 || limit > res.size()) {
    auto result = co_await (txn.*loadFunc)(begin, end, limit ? limit - res.size() : 0);
    CO_RETURN_ON_ERROR(result);
    for (auto &kv : result->kvs) {
      XLOGF_IF(FATAL,
               !kv.key.starts_with(prefix),
               "key {} not start with {}",
               toHexString(kv.key),
               toHexString(prefix));
      res.push_back(std::move(kv));
    }
    if (!result->hasMore) {
      break;
    }
    assert(!result->kvs.empty());
    beginKey = res.back().key;
    begin.key = beginKey;
    begin.inclusive = false;
  }
  co_return res;
}
}  // namespace hf3fs::kv
