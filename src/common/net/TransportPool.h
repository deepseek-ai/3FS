#pragma once

#include <folly/Likely.h>
#include <folly/ThreadLocal.h>

#include "common/net/Transport.h"
#include "common/utils/RobinHood.h"
#include "common/utils/Shards.h"

namespace hf3fs::net {

class IOWorker;

// A set of transports to same address for load balance.
class TransportSet {
 public:
  static constexpr uint32_t kDefaultMaxConnections = 1;

  // return a transport randomly.
  TransportPtr acquire(uint32_t idx);

  // add a new transport to this set.
  void add(TransportPtr tr, uint32_t idx);

  // remove a transport from this set.
  bool remove(TransportPtr tr);

  // get all transports.
  auto &transports() { return transports_; }

  // check all transports.
  uint32_t checkAll(Duration expiredTime);

  // drop all transports.
  uint32_t dropAll();

 protected:
  using TransportMap = std::map<TransportPtr, uint32_t>;

  void ensureSize(uint32_t idx);

  TransportMap::iterator erase(TransportMap::iterator it);

 private:
  TransportMap transports_{};
  std::vector<TransportMap::iterator> idxToTransport_{kDefaultMaxConnections, transports_.end()};
};

struct TransportCacheKey {
  Address addr;
  uint32_t idx;
  bool operator==(const TransportCacheKey &o) const { return addr == o.addr && idx == o.idx; }
};

}  // namespace hf3fs::net

template <>
struct std::hash<hf3fs::net::TransportCacheKey> {
  auto operator()(const hf3fs::net::TransportCacheKey &key) const {
    return std::hash<hf3fs::net::Address>{}(key.addr) ^ key.idx;
  }
};

namespace hf3fs::net {

// Maintain a map of addresses and transport.
class TransportPool {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(max_connections, TransportSet::kDefaultMaxConnections);
  };
  TransportPool(const Config &config)
      : config_(config) {}

  ~TransportPool();

  // add a transport into pool. [thread-safe]
  void add(TransportPtr tr);

  // remove a transport from pool. [thread-safe]
  void remove(TransportPtr tr);

  // get the transport corresponding to the specified address. [thread-safe]
  // the bool value in pair indicates whether connecting is required.
  std::pair<TransportPtr, bool> get(Address addr, IOWorker &io_worker);

  // drop all connections.
  void dropConnections(bool dropAll = true, bool dropIncome = false);

  // drop all connections to this addr.
  void dropConnections(Address addr);

  // drop connetions to peer.
  void checkConnections(Address addr, Duration expiredTime);

 private:
  const Config &config_;

  // sharding by address for better performance.
  constexpr static auto kShardsSize = 32u;
  using Map = robin_hood::unordered_map<Address, TransportSet>;
  Shards<Map, kShardsSize> shards_;

  // thread local cache for better performance.
  folly::ThreadLocal<robin_hood::unordered_map<TransportCacheKey, std::weak_ptr<Transport>>> tlsCache_;
};

}  // namespace hf3fs::net
