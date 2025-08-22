#pragma once

#include "client/mgmtd/MgmtdClient.h"
#include "common/app/NodeId.h"
#include "common/net/Client.h"
#include "common/utils/ConfigBase.h"

namespace hf3fs::core {
struct ServerLauncherConfig : public ConfigBase<ServerLauncherConfig> {
  CONFIG_ITEM(cluster_id, "");
  CONFIG_OBJ(ib_devices, net::IBDevice::Config);
  CONFIG_OBJ(client, net::Client::Config);
  CONFIG_OBJ(mgmtd_client, client::MgmtdClient::Config);
  CONFIG_ITEM(allow_dev_version, true);

 public:
  using Base = ConfigBase<ServerLauncherConfig>;
  using Base::init;

  void init(const String &filePath, bool dump, const std::vector<config::KeyValue> &updates);
};
}  // namespace hf3fs::core
