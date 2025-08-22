#pragma once

#include "LauncherUtils.h"
#include "client/mgmtd/MgmtdClient.h"
#include "common/net/Client.h"

namespace hf3fs::core::launcher {
struct MgmtdClientFetcher {
  MgmtdClientFetcher(String clusterId,
                     const net::Client::Config &clientCfg,
                     const client::MgmtdClient::Config &mgmtdClientCfg);

  template <typename ConfigT>
  MgmtdClientFetcher(const ConfigT &cfg)
      : MgmtdClientFetcher(cfg.cluster_id(), cfg.client(), cfg.mgmtd_client()) {}

  virtual ~MgmtdClientFetcher() { stopClient(); }

  Result<flat::ConfigInfo> loadConfigTemplate(flat::NodeType nodeType);
  Result<Void> ensureClientInited();
  void stopClient();

  virtual Result<Void> completeAppInfo(flat::AppInfo &appInfo) = 0;

  const String clusterId_;
  const net::Client::Config &clientCfg_;
  const client::MgmtdClient::Config &mgmtdClientCfg_;
  std::unique_ptr<net::Client> client_;
  std::shared_ptr<client::MgmtdClient> mgmtdClient_;
};
}  // namespace hf3fs::core::launcher
