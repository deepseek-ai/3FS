#pragma once

#include "common/net/ib/IBDevice.h"
#include "fdb/HybridKvEngineConfig.h"

namespace hf3fs::mgmtd {
struct MgmtdLauncherConfig : public ConfigBase<MgmtdLauncherConfig> {
  CONFIG_ITEM(cluster_id, "");
  CONFIG_ITEM(use_memkv, false);        // deprecated
  CONFIG_OBJ(fdb, kv::fdb::FDBConfig);  // deprecated
  CONFIG_OBJ(ib_devices, net::IBDevice::Config);
  CONFIG_OBJ(kv_engine, kv::HybridKvEngineConfig);
  CONFIG_ITEM(allow_dev_version, true);

 public:
  using ConfigBase<MgmtdLauncherConfig>::init;
  void init(const String &filePath, bool dump, const std::vector<config::KeyValue> &updates);
};
}  // namespace hf3fs::mgmtd
