#pragma once

#include "client/mgmtd/MgmtdClientForServer.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"

namespace hf3fs::client {

struct MgmtdClientWithConfig {
  MgmtdClient::Config config;
  String clusterId;
  stubs::ClientContextCreator clientContextCreator;
  std::shared_ptr<MgmtdClient> rawClient;
  std::unique_ptr<MgmtdClientForServer> client;

  MgmtdClientWithConfig(String clusterId,
                        stubs::ClientContextCreator clientContextCreator,
                        std::vector<net::Address> mgmtdServerAddrs) {
    config.set_mgmtd_server_addresses(to_named(mgmtdServerAddrs));
    config.set_enable_auto_refresh(false);
    config.set_enable_auto_heartbeat(false);
    config.set_enable_auto_extend_client_session(false);

    this->clusterId = std::move(clusterId);
    this->clientContextCreator = std::move(clientContextCreator);

    auto stubFactory = std::make_unique<stubs::RealStubFactory<mgmtd::MgmtdServiceStub>>(this->clientContextCreator);
    rawClient = std::make_unique<MgmtdClient>(this->clusterId, std::move(stubFactory), config);
    client = std::make_unique<MgmtdClientForServer>(rawClient);
  }

  MgmtdClientForServer *operator->() const { return client.get(); }

  MgmtdClientForServer &operator*() const { return *client; }
};

}  // namespace hf3fs::client
