#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <folly/Synchronized.h>
#include <folly/lang/Bits.h>
#include <folly/logging/xlog.h>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "common/utils/String.h"

namespace hf3fs::kv::mem {

inline int32_t memKvDefaultLimit = 25;

class MemKV : public folly::MoveOnly {
 public:
  struct VersionstampedKV {
    std::string key;
    std::string value;
    bool onKey;
    uint32_t offset;

    static VersionstampedKV versionstampedKey(std::string key, uint32_t offset, std::string value) {
      return {key, value, true, offset};
    }

    static VersionstampedKV versionstampedValue(std::string key, std::string value, uint32_t offset) {
      return {key, value, false, offset};
    }
  };

  Result<std::optional<String>> get(std::string_view key, int64_t readVersion) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = map_.find(String(key));
    if (it == map_.end()) {
      return std::optional<String>();
    }

    auto version_it = it->second.upper_bound(readVersion);
    if (version_it != it->second.begin()) {
      --version_it;
      if (version_it->second.has_value()) {
        return std::make_optional(version_it->second.value());
      }
    }
    return std::optional<String>();
  }

  Result<IReadOnlyTransaction::GetRangeResult> getRange(const IReadOnlyTransaction::KeySelector &begin,
                                                        const IReadOnlyTransaction::KeySelector &end,
                                                        int32_t limit,
                                                        int64_t readVersion) const {
    if (limit < 1) {
      limit = memKvDefaultLimit;
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<IReadOnlyTransaction::KeyValue> kvs;

    if (begin.key > end.key || (begin.key == end.key && !(begin.inclusive && end.inclusive))) {
      return IReadOnlyTransaction::GetRangeResult(std::move(kvs), false);
    }

    auto begin_it = begin.inclusive ? map_.lower_bound(begin.key) : map_.upper_bound(begin.key);
    auto end_it = end.inclusive ? map_.upper_bound(end.key) : map_.lower_bound(end.key);
    while (begin_it != end_it && kvs.size() < (size_t)limit) {
      auto version_it = begin_it->second.upper_bound(readVersion);
      if (version_it != begin_it->second.begin()) {
        --version_it;
        if (version_it->second.has_value()) {
          kvs.emplace_back(begin_it->first, version_it->second.value());
        }
      }
      ++begin_it;
    }

    return IReadOnlyTransaction::GetRangeResult(std::move(kvs), begin_it != end_it);
  }

  Result<int64_t> commit(int64_t readVersion,
                         const std::unordered_set<String> &readKeys,
                         const std::vector<std::pair<String, String>> readRanges,
                         const std::vector<std::pair<String, std::optional<String>>> changes,
                         const std::vector<VersionstampedKV> versionstampedChanges,
                         const std::set<String> writeConflicts) {
    // if transaction contains no updates, don't need commit.
    if (changes.empty() && versionstampedChanges.empty() && writeConflicts.empty()) {
      return -1;  // return a invalid version
    }
    if (readVersion < 0) {
      return makeError(TransactionCode::kFailed, "MemKV transaction invalid version!");
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    // check conflict
    for (auto &key : readKeys) {
      auto it = map_.find(key);
      if (it != map_.end()) {
        auto version_it = it->second.upper_bound(readVersion);
        if (version_it != it->second.end()) {
          return makeError(TransactionCode::kConflict, "MemKV transaction conflicts!");
        }
      }
    }
    for (auto &range : readRanges) {
      auto it = map_.lower_bound(range.first);
      while (it != map_.end() && it->first < range.second) {
        auto version_it = it->second.upper_bound(readVersion);
        if (version_it != it->second.end()) {
          return makeError(TransactionCode::kConflict, "MemKV transaction conflicts!");
        }
        it++;
      }
    }

    auto version = ++version_;
    uint16_t seq = 0;
    for (auto change : versionstampedChanges) {
      Versionstamp(version, seq++).update(change);
      XLOGF_IF(FATAL, change.onKey && map_.contains(change.key), "shouldn't happen");
      map_[change.key][version] = change.value;
    }
    for (auto &change : changes) {
      if (change.second.has_value()) {
        map_[change.first][version] = change.second.value();
      } else {
        map_[change.first][version] = std::nullopt;
      }
    }
    for (const auto &conflict : writeConflicts) {
      if (map_.contains(conflict)) {
        auto value = map_[conflict].rbegin()->second;
        map_[conflict][version] = value;
      } else {
        map_[conflict][version] = std::nullopt;
      }
    }
    return version;
  }

  int64_t version() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return version_;
  }

  void backup() { states_.lock()->push(version()); }

  void restore() {
    auto guard = states_.lock();
    auto version = guard->front();
    guard->pop();
    restore(version);
  }

 private:
  struct Versionstamp {
    std::array<uint8_t, 8> version;
    std::array<uint8_t, 2> seq;

    Versionstamp(uint64_t version, uint16_t seq)
        : version(folly::bit_cast<std::array<uint8_t, 8>>(folly::Endian::big64(version))),
          seq(folly::bit_cast<std::array<uint8_t, 2>>(folly::Endian::big16(seq))) {}

    void update(VersionstampedKV &kv) const {
      auto &str = kv.onKey ? kv.key : kv.value;
      auto offset = kv.offset;
      XLOGF_IF(FATAL, offset + 10 > str.size(), "{} {}", offset, str.size());
      memcpy(str.data() + offset, (char *)this, 10);
    }
  };
  static_assert(sizeof(Versionstamp) == 10);

  void restore(int64_t version) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    version_ = version;

    // remove all modification after version
    auto iter = map_.begin();
    while (iter != map_.end()) {
      auto &values = iter->second;
      auto pos = values.lower_bound(version_ + 1);
      if (pos != values.end()) {
        values.erase(pos, values.end());
      }
      if (values.empty()) {
        map_.erase(iter++);
      } else {
        iter++;
      }
    }
  }

  void clear() {
    std::unique_lock lock(mutex_);
    map_.clear();
    version_ = 0;
  }

 private:
  mutable std::shared_mutex mutex_;
  folly::Synchronized<std::queue<int64_t>, std::mutex> states_;
  std::map<String, std::map<int64_t, std::optional<String>>, std::less<>> map_;
  int64_t version_ = 0;
};

}  // namespace hf3fs::kv::mem
