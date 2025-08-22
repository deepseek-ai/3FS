#pragma once

#include "common/utils/ConfigBase.h"
#include "common/utils/LockManager.h"
#include "common/utils/RobinHood.h"
#include "common/utils/UtcTime.h"
#include "fbs/core/user/User.h"

namespace hf3fs::core {
using flat::Gid;
using flat::Uid;
using flat::UserAttr;
using flat::UserInfo;

class UserCache {
 public:
  struct Config : ConfigBase<Config> {
    CONFIG_ITEM(buckets, 127u, ConfigCheckers::isPositivePrime<uint32_t>);
    CONFIG_HOT_UPDATED_ITEM(exist_ttl, 5_min);
    CONFIG_HOT_UPDATED_ITEM(inexist_ttl, 10_s);
  };

  explicit UserCache(const Config &cfg)
      : cfg_(cfg),
        lockManager_(cfg.buckets()),
        buckets_(cfg.buckets()) {}

  struct GetResult {
    std::optional<UserAttr> attr;
    bool marked = false;

    static GetResult unmarkedNotExist() { return {std::nullopt, false}; }
    static GetResult markedNotExist() { return {std::nullopt, true}; }
    static GetResult exist(UserAttr attr) { return {std::move(attr), false}; }
  };

  GetResult get(Uid id) {
    auto lock = lockManager_.lock(id);
    auto now = SteadyClock::now();
    auto &bucket = buckets_[lockManager_.idx(id)];

    auto it = bucket.find(id);
    if (it == bucket.end()) return GetResult::unmarkedNotExist();

    auto &entry = it->second;
    if (expired(entry, now)) {
      bucket.erase(it);
      return GetResult::unmarkedNotExist();
    }

    if (entry.attr)
      return GetResult::exist(*entry.attr);
    else
      return GetResult::markedNotExist();
  }

  void set(Uid id, std::optional<UserAttr> attr) {
    auto lock = lockManager_.lock(id);
    auto &bucket = buckets_[lockManager_.idx(id)];
    auto &entry = bucket[id];
    auto now = SteadyClock::now();
    entry.setTs = now;
    entry.attr = std::move(attr);
  }

  void clear(Uid id) {
    auto lock = lockManager_.lock(id);
    auto &bucket = buckets_[lockManager_.idx(id)];
    bucket.erase(id);
  }

  void clear() {
    for (uint32_t i = 0; i < lockManager_.numBuckets(); ++i) {
      auto lock = lockManager_.lock_at(i);
      auto &bucket = buckets_[i];
      bucket.clear();
    }
  }

  void clearRetired() {
    std::vector<Uid> uids;
    for (uint32_t i = 0; i < lockManager_.numBuckets(); ++i) {
      auto lock = lockManager_.lock_at(i);
      auto &bucket = buckets_[i];
      auto now = SteadyClock::now();
      for (const auto &[id, entry] : bucket) {
        if (expired(entry, now)) uids.push_back(id);
      }
    }
    if (!uids.empty()) {
      for (uint32_t i = 0; i < lockManager_.numBuckets(); ++i) {
        auto lock = lockManager_.lock_at(i);
        auto &bucket = buckets_[i];
        auto now = SteadyClock::now();
        for (auto id : uids) {
          auto it = bucket.find(id);
          if (expired(it->second, now)) bucket.erase(it);
        }
      }
    }
  }

 private:
  struct Entry {
    std::optional<UserAttr> attr;
    SteadyTime setTs;
  };

  bool expired(const Entry &entry, SteadyTime now) const {
    auto timeout = entry.attr ? cfg_.exist_ttl().asUs() : cfg_.inexist_ttl().asUs();
    return now - entry.setTs >= timeout;
  }

  using Bucket = robin_hood::unordered_node_map<Uid, Entry>;

  const Config &cfg_;
  UniqueLockManager lockManager_;
  std::vector<Bucket> buckets_;
};
}  // namespace hf3fs::core
