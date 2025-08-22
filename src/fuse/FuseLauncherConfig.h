#pragma once

#include "client/mgmtd/MgmtdClientForClient.h"
#include "common/app/NodeId.h"
#include "common/net/Client.h"
#include "common/utils/ConfigBase.h"

namespace hf3fs::fuse {
struct FuseLauncherConfig : public ConfigBase<FuseLauncherConfig> {
  CONFIG_ITEM(cluster_id, "");
  CONFIG_OBJ(ib_devices, net::IBDevice::Config);
  CONFIG_OBJ(client, net::Client::Config);
  CONFIG_OBJ(mgmtd_client, client::MgmtdClientForClient::Config);
  CONFIG_ITEM(mountpoint, "");
  CONFIG_ITEM(allow_other, true);
  CONFIG_ITEM(token_file, "");

 public:
  using Base = ConfigBase<FuseLauncherConfig>;
  using Base::init;

  void init(const String &filePath, bool dump, const std::vector<config::KeyValue> &updates);
};
}  // namespace hf3fs::fuse
