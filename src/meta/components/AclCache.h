#pragma once

#include <array>
#include <folly/Random.h>
#include <folly/Synchronized.h>
#include <folly/container/EvictingCacheMap.h>
#include <optional>

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
namespace hf3fs::meta::server {

class AclCache {
 public:
  AclCache(size_t cacheSize) {
    for (size_t i = 0; i < kNumShards; i++) {
      shardedMaps_.emplace_back(
          folly::EvictingCacheMap<InodeId, CacheEntry>(std::max(cacheSize / kNumShards, 1ul << 10), 128));
    }
  }

  std::optional<Acl> get(InodeId inode, Duration ttl) {
    static monitor::CountRecorder hit("meta_server.aclcache_hit");
    static monitor::CountRecorder miss("meta_server.aclcache_miss");

    if (ttl.count() == 0) {
      return std::nullopt;
    }
    std::optional<CacheEntry> cached;
    {
      auto &shard = getShard(inode);
      auto guard = shard.lock();
      auto iter = guard->find(inode);
      if (iter != guard->end()) {
        cached = iter->second;
      }
    }

    if (!cached.has_value()) {
      miss.addSample(1);
      return std::nullopt;
    }
    auto deadline = cached->timestamp + ttl * folly::Random::randDouble(0.8, 1.0);
    if (deadline < SteadyClock::now()) {
      miss.addSample(1);
      return std::nullopt;
    }
    hit.addSample(1);
    return cached->acl;
  }

  void set(InodeId inode, Acl acl) {
    auto &shard = getShard(inode);
    shard.lock()->set(inode, {SteadyClock::now(), acl});
  }

  void invalid(InodeId inode) { getShard(inode).lock()->erase(inode); }

 private:
  static constexpr auto kNumShards = 32u;

  struct CacheEntry {
    SteadyTime timestamp;
    Acl acl;
  };
  using CacheMap = folly::Synchronized<folly::EvictingCacheMap<InodeId, CacheEntry>, std::mutex>;

  CacheMap &getShard(InodeId inode) {
    auto shardId = inode.u64() % kNumShards;
    return shardedMaps_[shardId];
  }

  std::vector<CacheMap> shardedMaps_;
};

}  // namespace hf3fs::meta::server
