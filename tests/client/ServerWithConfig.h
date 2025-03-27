#pragma once

#include "client/mgmtd/MgmtdClientForServer.h"
#include "common/kv/mem/MemKVEngine.h"
#include "meta/service/MetaServer.h"
#include "mgmtd/MgmtdServer.h"

namespace hf3fs {

template <typename ServerT>
struct ServerWithConfig {
  typename ServerT::Config config;
  String clusterId;
  flat::NodeId nodeId;
  std::unique_ptr<ServerT> server;
  String hostname;
  uint32_t pid = 1;

  ServerWithConfig(String clusterId, flat::NodeId nodeId) {
    this->clusterId = std::move(clusterId);
    this->nodeId = nodeId;

    auto &baseConfig = config.base();
    for (size_t i = 0; i < baseConfig.groups_length(); ++i) {
      baseConfig.groups(i).set_network_type(net::Address::LOCAL);
      baseConfig.groups(i).listener().set_listen_port(0);
      baseConfig.groups(i).listener().set_reuse_port(true);
    }
  }

  Result<Void> start(std::shared_ptr<kv::IKVEngine> kvEngine) {
    assert(!server);
    auto server = std::make_unique<ServerT>(config);
    RETURN_ON_ERROR(server->setup());

    flat::AppInfo appInfo;
    appInfo.nodeId = nodeId;
    appInfo.clusterId = clusterId;
    appInfo.hostname = fmt::format("hostname.{}", nodeId.toUnderType());
    appInfo.pid = pid++;

    std::vector<net::Address> serverAddrs;
    for (auto &group : server->groups()) {
      appInfo.serviceGroups.emplace_back(group->serviceNameList(), group->addressList());
      serverAddrs.insert(serverAddrs.end(), group->addressList().begin(), group->addressList().end());
    }

    RETURN_ON_ERROR(server->start(appInfo, std::move(kvEngine)));
    this->server = std::move(server);
    return Void{};
  }

  void stop() {
    if (server) server->stopAndJoin();
    server.reset();
  }

  Result<Void> restart(std::shared_ptr<kv::IKVEngine> kvEngine) {
    stop();
    return start(std::move(kvEngine));
  }

  std::vector<net::Address> collectAddressList(std::string_view serviceName) const {
    std::vector<net::Address> serverAddrs;
    for (const auto &group : server->groups()) {
      const auto &names = group->serviceNameList();
      const auto &addrs = group->addressList();
      if (std::find(names.begin(), names.end(), serviceName) != names.end()) {
        serverAddrs.insert(serverAddrs.end(), addrs.begin(), addrs.end());
      }
    }
    return serverAddrs;
  }
};

using MgmtdServerWithConfig = ServerWithConfig<mgmtd::MgmtdServer>;

struct MetaServerWithConfig : ServerWithConfig<meta::server::MetaServer> {
  using Base = ServerWithConfig<meta::server::MetaServer>;

  MetaServerWithConfig(String clusterId, flat::NodeId nodeId, std::vector<net::Address> mgmtdServerAddrs)
      : Base(std::move(clusterId), nodeId) {
    auto &mgmtdClientConfig = config.mgmtd_client();
    mgmtdClientConfig.set_mgmtd_server_addresses(to_named(mgmtdServerAddrs));
  }
};

}  // namespace hf3fs
