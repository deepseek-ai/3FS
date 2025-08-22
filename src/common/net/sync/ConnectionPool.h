#pragma once

#include <map>

#include "common/net/Network.h"
#include "common/net/tcp/TcpSocket.h"
#include "common/utils/BoundedQueue.h"
#include "common/utils/ConfigBase.h"

namespace hf3fs::net::sync {

class ConnectionPool {
 public:
  struct Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(max_connection_num, 16ul);
    CONFIG_HOT_UPDATED_ITEM(tcp_connect_timeout, 1000_ms);
  };

  ConnectionPool(const Config &config)
      : config_(config) {}

  Result<std::unique_ptr<TcpSocket>> acquire(Address addr);

  void restore(Address addr, std::unique_ptr<TcpSocket> socket);

 private:
  using Connections = BoundedQueue<std::unique_ptr<TcpSocket>>;
  Connections *getConnections(Address addr);

 private:
  [[maybe_unused]] const Config &config_;
  std::mutex mutex_;
  robin_hood::unordered_map<Address, std::unique_ptr<Connections>> map_;
  folly::ThreadLocal<robin_hood::unordered_map<Address, Connections *>> tlsCache_;
};

}  // namespace hf3fs::net::sync
