#pragma once

#include <algorithm>
#include <cassert>
#include <folly/Likely.h>
#include <folly/Synchronized.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/logging/xlog.h>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/kv/mem/MemKV.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/RandomUtils.h"
#include "common/utils/Result.h"
#include "common/utils/Status.h"
#include "common/utils/String.h"

#define FAULT_INJECTION_ON_GET(op_name)                                                                              \
  do {                                                                                                               \
    if (FAULT_INJECTION()) {                                                                                         \
      auto delayMs = folly::Random::rand32(10, 100);                                                                 \
      auto errCode =                                                                                                 \
          hf3fs::RandomUtils::randomSelect(std::vector<int>{TransactionCode::kThrottled, TransactionCode::kTooOld}); \
      XLOGF(WARN, "Inject fault on " #op_name ", errCode: {}, delayMs: {}.", errCode, delayMs);                      \
      co_await folly::coro::sleep(std::chrono::milliseconds(delayMs));                                               \
      co_return makeError(errCode, "Inject fault on " #op_name);                                                     \
    }                                                                                                                \
  } while (0)

#define FAULT_INJECTION_ON_COMMIT(db_name, injectMaybeCommitted)                                                     \
  do {                                                                                                               \
    if (FAULT_INJECTION()) {                                                                                         \
      auto delayMs = folly::Random::rand32(10, 100);                                                                 \
      auto errCode = hf3fs::RandomUtils::randomSelect(                                                               \
          std::vector<int>{TransactionCode::kConflict, TransactionCode::kTooOld, TransactionCode::kMaybeCommitted}); \
      XLOGF(WARN, "Inject fault on commit, errCode: {}, delayMs: {}.", errCode, delayMs);                            \
      if (errCode == TransactionCode::kMaybeCommitted && folly::Random::oneIn(2)) {                                  \
        injectMaybeCommitted = true;                                                                                 \
        XLOGF(WARN, "Inject maybeCommitted on commit, commit transaction in " #db_name ".");                         \
      } else {                                                                                                       \
        co_return makeError(errCode, "Inject fault on commit.");                                                     \
      }                                                                                                              \
    }                                                                                                                \
  } while (0)

namespace hf3fs::kv {

class MemTransaction : public IReadWriteTransaction {
 public:
  MemTransaction(mem::MemKV &mem)
      : mem_(mem),
        readVersion_(mem.version()) {}

  CoTryTask<std::optional<String>> snapshotGet(std::string_view key) override {
    FAULT_INJECTION_ON_GET(snapshotGet);
    co_return getImpl(key, true);
  }

  CoTryTask<GetRangeResult> snapshotGetRange(const KeySelector &begin, const KeySelector &end, int32_t limit) override {
    FAULT_INJECTION_ON_GET(snapshotGetRange);
    co_return getRangeImpl(begin, end, limit, true);
  }

  CoTryTask<void> cancel() override {
    std::scoped_lock<std::mutex> guard(mutex_);
    canceled_ = true;
    co_return Void{};
  }

  CoTryTask<void> addReadConflict(std::string_view key) override {
    std::scoped_lock<std::mutex> guard(mutex_);
    if (canceled_) {
      co_return makeError(TransactionCode::kCanceled, "Canceled transaction!");
    }
    if (changes_.contains(key)) {
      co_return Void{};
    }
    readKeys_.insert(String(key));
    co_return Void{};
  }

  CoTryTask<void> addReadConflictRange(std::string_view begin, std::string_view end) override {
    std::scoped_lock<std::mutex> guard(mutex_);
    if (canceled_) {
      co_return makeError(TransactionCode::kCanceled, "Canceled transaction!");
    }
    readRanges_.push_back({std::string(begin), std::string(end)});
    co_return Void{};
  }

  CoTryTask<std::optional<String>> get(std::string_view key) override {
    FAULT_INJECTION_ON_GET(get);
    co_return getImpl(key, false);
  }

  CoTryTask<GetRangeResult> getRange(const KeySelector &begin, const KeySelector &end, int32_t limit) override {
    FAULT_INJECTION_ON_GET(getRange);
    co_return getRangeImpl(begin, end, limit, false);
  }

  CoTryTask<void> set(std::string_view key, std::string_view value) override {
    std::scoped_lock<std::mutex> guard(mutex_);
    if (canceled_) {
      co_return makeError(TransactionCode::kCanceled, "Canceled transaction!");
    }
    changes_[String(key)] = value;
    co_return Void{};
  }

  CoTryTask<void> setVersionstampedKey(std::string_view key, uint32_t offset, std::string_view value) override {
    std::scoped_lock<std::mutex> guard(mutex_);
    if (canceled_) {
      co_return makeError(TransactionCode::kCanceled, "Canceled transaction!");
    }
    if (offset + sizeof(kv::Versionstamp) > key.size()) {
      co_return makeError(
          StatusCode::kInvalidArg,
          fmt::format("setVersionstampedKey: {} + sizeof(kv::Versionstamp) > key.size {}", offset, key.size()));
    }
    versionstampedChanges_.push_back(
        mem::MemKV::VersionstampedKV::versionstampedKey(std::string(key), offset, std::string(value)));
    co_return Void{};
  }

  CoTryTask<void> setVersionstampedValue(std::string_view key, std::string_view value, uint32_t offset) override {
    std::scoped_lock<std::mutex> guard(mutex_);
    if (canceled_) {
      co_return makeError(TransactionCode::kCanceled, "Canceled transaction!");
    }
    if (offset + sizeof(kv::Versionstamp) > value.size()) {
      co_return makeError(
          StatusCode::kInvalidArg,
          fmt::format("setVersionstampedValue: {} + sizeof(kv::Versionstamp) > value.size {}", offset, value.size()));
    }
    versionstampedChanges_.push_back(
        mem::MemKV::VersionstampedKV::versionstampedValue(std::string(key), std::string(value), offset));
    co_return Void{};
  }

  // Check given keys are in transaction's conflict set.
  bool checkConflictSet(const std::vector<String> &readConflict,
                        const std::vector<String> &writeConflict,
                        bool exactly = false) {
    std::scoped_lock<std::mutex> guard(mutex_);
    for (auto &key : readConflict) {
      if (!readKeys_.contains(key)) {
        XLOGF(ERR, "Read conflict set doesn't contains key {}", key);
        return false;
      }
    }
    for (auto &key : writeConflict) {
      auto iter = std::find_if(changes_.begin(), changes_.end(), [&](auto &iter) -> bool { return iter.first == key; });
      if (iter == changes_.end()) {
        XLOGF(ERR, "Write conflict set doesn't contains key {}", key);
        return false;
      }
    }
    if (!exactly) {
      return true;
    }

    // transaction's conflict set should only given keys
    for (auto &key : readKeys_) {
      if (std::find(readConflict.begin(), readConflict.end(), key) == readConflict.end()) {
        XLOGF(ERR, "Read conflict contains unexpected key {}", key);
        return false;
      }
    }
    for (auto &change : changes_) {
      if (std::find(writeConflict.begin(), writeConflict.end(), change.first) == writeConflict.end()) {
        XLOGF(ERR, "Write conflict contains unexpected key {}", change.first);
        return false;
      }
    }
    return true;
  }

  CoTryTask<void> clear(std::string_view key) override {
    std::scoped_lock<std::mutex> guard(mutex_);
    if (canceled_) {
      co_return makeError(TransactionCode::kCanceled, "Canceled transaction!");
    }
    changes_[String(key)] = std::optional<String>();
    co_return Void{};
  }

  CoTryTask<void> commit() override {
    bool injectMaybeCommitted = false;
    FAULT_INJECTION_ON_COMMIT(MemKV, injectMaybeCommitted);

    std::scoped_lock<std::mutex> guard(mutex_);
    if (canceled_) {
      co_return makeError(TransactionCode::kCanceled, "Canceled transaction!");
    }

    std::vector<std::pair<String, std::optional<String>>> vec;
    vec.reserve(changes_.size());
    for (auto iter : changes_) {
      vec.emplace_back(iter.first, iter.second);
    }
    auto result = mem_.commit(readVersion(), readKeys_, readRanges_, vec, versionstampedChanges_, writeConflicts_);
    if (UNLIKELY(injectMaybeCommitted)) {
      XLOGF(ERR,
            "Inject mayebeCommitted error after commit, MemKV commit result is {}.",
            result.hasError() ? result.error().describe() : "OK");
      co_return makeError(TransactionCode::kMaybeCommitted, "Fault injection commit unknown result.");
    }
    CO_RETURN_ON_ERROR(result);
    commitVersion_ = *result;
    co_return Void{};
  }

  void setReadVersion(int64_t version) override {
    std::scoped_lock<std::mutex> guard(mutex_);
    if (version >= 0) {
      readVersion_ = version;
    }
  }

  int64_t getCommittedVersion() override {
    std::scoped_lock<std::mutex> guard(mutex_);
    return commitVersion_;
  };

  void reset() override {
    std::scoped_lock<std::mutex> guard(mutex_);
    readVersion_ = -1;
    commitVersion_ = -1;
    canceled_ = false;
    readKeys_.clear();
    readRanges_.clear();
    changes_.clear();
    versionstampedChanges_.clear();
    writeConflicts_.clear();
  }

  // check txn1 updated txn2's read set
  static bool checkConflict(MemTransaction &txn1, MemTransaction &txn2) {
    std::scoped_lock<std::mutex> guard1(txn1.mutex_);
    std::scoped_lock<std::mutex> guard2(txn2.mutex_);

    for (auto &change : txn1.changes_) {
      auto &key = change.first;
      if (std::find(txn2.readKeys_.begin(), txn2.readKeys_.end(), key) != txn2.readKeys_.end()) {
        return true;
      }
      for (auto &range : txn2.readRanges_) {
        if (key >= range.first && key < range.second) {
          return true;
        }
      }
    }
    return false;
  }

  mem::MemKV &kv() { return mem_; }

 private:
  int64_t readVersion() {
    if (readVersion_ < 0) {
      readVersion_ = mem_.version();
    }
    return readVersion_;
  }

  Result<std::optional<String>> getImpl(std::string_view key, bool snapshot) {
    std::scoped_lock<std::mutex> guard(mutex_);
    if (canceled_) {
      return makeError(TransactionCode::kCanceled, "Canceled transaction!");
    }

    if (changes_.count(key)) {
      return changes_.at(String(key));
    }
    if (!snapshot) {
      readKeys_.insert(String(key));
    }
    return mem_.get(key, readVersion());
  }

  Result<GetRangeResult> getRangeImpl(const KeySelector &begin, const KeySelector &end, int32_t limit, bool snapshot) {
    std::scoped_lock<std::mutex> guard(mutex_);
    if (canceled_) {
      return makeError(TransactionCode::kCanceled, "Canceled transaction!");
    }
    // always get all kvs in range
    auto result = mem_.getRange(begin, end, std::numeric_limits<int32_t>::max(), readVersion());
    RETURN_ON_ERROR(result);

    auto changeIt = changes_.lower_bound(begin.key);
    if (changeIt != changes_.end() && !begin.inclusive) ++changeIt;

    if (changeIt != changes_.end()) {
      std::vector<KeyValue> newKvs;
      auto originIt = result->kvs.begin();
      auto originEnded = [&] { return originIt == result->kvs.end(); };
      auto changeEnded = [&] {
        if (changeIt == changes_.end()) return true;
        if (end.inclusive && changeIt->first > end.key) return true;
        if (!end.inclusive && changeIt->first >= end.key) return true;
        if (result->hasMore) {
          assert(!result->kvs.empty());
          if (changeIt->first > result->kvs.back().key) return true;
        }
        return false;
      };
      auto pushChange = [&] {
        if (changeIt->second.has_value()) newKvs.emplace_back(changeIt->first, *changeIt->second);
      };
      for (;;) {
        if (originEnded() || changeEnded()) break;
        if (originIt->key < changeIt->first) {
          newKvs.push_back(*originIt);
          ++originIt;
        } else if (originIt->key > changeIt->first) {
          pushChange();
          ++changeIt;
        } else {
          pushChange();
          ++changeIt;
          ++originIt;
        }
      }
      for (; !originEnded(); ++originIt) {
        newKvs.push_back(*originIt);
      }
      for (; !changeEnded(); ++changeIt) {
        pushChange();
      }
      result->kvs = std::move(newKvs);
    }

    if (limit < 1) {
      limit = mem::memKvDefaultLimit;
    }
    if (result->kvs.size() > (size_t)limit) {
      result->kvs = std::vector(&result->kvs[0], &result->kvs[(size_t)limit]);
      result->hasMore = true;
      assert(result->kvs.size() == (size_t)limit);
    }

    String rangeBegin = begin.inclusive ? String(begin.key) : TransactionHelper::keyAfter(begin.key);
    String rangeEnd;
    if (result->hasMore) {
      rangeEnd = TransactionHelper::keyAfter(result->kvs.end()[-1].key);
    } else {
      rangeEnd = end.inclusive ? TransactionHelper::keyAfter(end.key) : String(end.key);
    }
    if (!snapshot) {
      readRanges_.push_back({rangeBegin, rangeEnd});
    }

    return result;
  }

  std::mutex mutex_;
  mem::MemKV &mem_;
  bool canceled_ = false;
  int64_t readVersion_ = -1;
  int64_t commitVersion_ = -1;
  std::unordered_set<String> readKeys_;
  std::vector<std::pair<String, String>> readRanges_;
  std::map<String, std::optional<String>, std::less<>> changes_;
  std::vector<mem::MemKV::VersionstampedKV> versionstampedChanges_;
  std::set<String> writeConflicts_;
};

}  // namespace hf3fs::kv
