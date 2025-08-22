#pragma once

#include "CommonMgmtdClient.h"
#include "IMgmtdClientForClient.h"

namespace hf3fs::client {
class MgmtdClientForClient : public CommonMgmtdClient<IMgmtdClientForClient> {
 public:
  struct Config : MgmtdClient::Config {
    Config() {
      set_enable_auto_refresh(true);
      set_enable_auto_heartbeat(false);
      set_enable_auto_extend_client_session(true);
    }
  };

  explicit MgmtdClientForClient(std::shared_ptr<MgmtdClient> client)
      : CommonMgmtdClient(std::move(client)) {}

  MgmtdClientForClient(String clusterId,
                       std::unique_ptr<MgmtdClient::MgmtdStubFactory> stubFactory,
                       const Config &config)
      : CommonMgmtdClient(std::make_shared<MgmtdClient>(std::move(clusterId), std::move(stubFactory), config)) {}

  CoTryTask<void> extendClientSession() final { return client_->extendClientSession(); }

  void setClientSessionPayload(ClientSessionPayload payload) { client_->setClientSessionPayload(std::move(payload)); }

  void setConfigListener(ConfigListener listener) final { client_->setClientConfigListener(std::move(listener)); }
};
}  // namespace hf3fs::client
