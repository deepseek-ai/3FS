#pragma once

#include <atomic>
#include <folly/io/coro/ServerSocket.h>
#include <thread>

#include "common/net/IOWorker.h"
#include "common/net/Network.h"
#include "common/net/Transport.h"
#include "common/net/ib/IBConnectService.h"
#include "common/net/ib/IBSocket.h"
#include "common/utils/ConfigBase.h"

namespace hf3fs::net {

class ServiceGroup;

class Listener {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_ITEM(listen_port, uint16_t{0});
    CONFIG_ITEM(reuse_port, false);
    CONFIG_ITEM(listen_queue_depth, 4096u);
    CONFIG_ITEM(filter_list, std::set<std::string>{});  // use all available network cards if filter is empty.
    CONFIG_ITEM(rdma_listen_ethernet, true);            // support setup RDMA connection with Ethernet by default.
    CONFIG_ITEM(domain_socket_index, 1u);
    // Configurable TCP nic prefix. Currently, "en", "eth", "bond", and "xgbe" are regarded as TCP network cards. If
    // your nic is not included in the list above, you can configure this option. Note that when the network type is set
    // to RDMA, it still relies on TCP network for connection establishment and handshaking.
    CONFIG_ITEM(custom_tcp_nic_prefix, "");
    CONFIG_HOT_UPDATED_ITEM(rdma_accept_timeout, 15_s);
  };

  Listener(const Config &config,
           const IBSocket::Config &ibconfig,
           IOWorker &ioWorker,
           folly::IOThreadPoolExecutor &connThreadPool,
           Address::Type networkType);
  ~Listener() { stopAndJoin(); }

  // setup listener.
  Result<Void> setup();

  // start listening.
  Result<Void> start(ServiceGroup &group);

  // stop listening.
  void stopAndJoin();

  // get listening address list.
  const std::vector<Address> &addressList() const { return addressList_; }

 protected:
  // do listen with calcellation support.
  CoTask<void> listen(folly::coro::ServerSocket socket);

  // accept a TCP connection.
  CoTask<void> acceptTCP(std::unique_ptr<folly::coro::Transport> tr);

  // accept a RDMA connection.
  void acceptRDMA(std::unique_ptr<IBSocket> socket);
  CoTask<void> checkRDMA(std::weak_ptr<Transport> weak);

  // release a TCP connection.
  CoTask<void> release(folly::coro::ServerSocket /* socket */);

 private:
  const Config &config_;
  const IBSocket::Config &ibconfig_;
  IOWorker &ioWorker_;
  folly::IOThreadPoolExecutor &connThreadPool_;
  Address::Type networkType_;

  CancellationSource cancel_;
  std::vector<Address> addressList_;
  std::vector<folly::coro::ServerSocket> serverSockets_;
  std::atomic<size_t> running_{0};
};

}  // namespace hf3fs::net
