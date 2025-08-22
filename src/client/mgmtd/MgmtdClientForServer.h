#pragma once

#include "CommonMgmtdClient.h"
#include "IMgmtdClientForServer.h"

namespace hf3fs::client {
class MgmtdClientForServer : public CommonMgmtdClient<IMgmtdClientForServer> {
 public:
  struct Config : MgmtdClient::Config {
    Config() {
      set_enable_auto_refresh(true);
      set_enable_auto_heartbeat(true);
      set_enable_auto_extend_client_session(false);
    }
  };

  explicit MgmtdClientForServer(std::shared_ptr<MgmtdClient> client)
      : CommonMgmtdClient(std::move(client)) {}

  MgmtdClientForServer(String clusterId,
                       std::unique_ptr<MgmtdClient::MgmtdStubFactory> stubFactory,
                       const Config &config)
      : CommonMgmtdClient(std::make_shared<MgmtdClient>(std::move(clusterId), std::move(stubFactory), config)) {}

  CoTryTask<mgmtd::HeartbeatRsp> heartbeat() final { return client_->heartbeat(); }

  Result<Void> triggerHeartbeat() final { return client_->triggerHeartbeat(); }

  void setAppInfoForHeartbeat(flat::AppInfo info) final { client_->setAppInfoForHeartbeat(std::move(info)); }

  void setConfigListener(ConfigListener listener) final { client_->setServerConfigListener(std::move(listener)); }

  void updateHeartbeatPayload(HeartbeatPayload payload) final { client_->updateHeartbeatPayload(std::move(payload)); }
};
}  // namespace hf3fs::client
