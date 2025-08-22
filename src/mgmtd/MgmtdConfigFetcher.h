#pragma once

#include "common/app/AppInfo.h"
#include "common/kv/IKVEngine.h"
#include "fbs/mgmtd/ConfigInfo.h"
#include "fdb/HybridKvEngineConfig.h"

namespace hf3fs::mgmtd {
class MgmtdServer;
struct MgmtdConfigFetcher {
  template <typename ConfigT>
  explicit MgmtdConfigFetcher(const ConfigT &cfg) {
    init(cfg.kv_engine(), cfg.use_memkv(), cfg.fdb());
  }

  Result<flat::ConfigInfo> loadConfigTemplate(flat::NodeType nodeType);
  Result<Void> completeAppInfo(flat::AppInfo &appInfo);

  Result<Void> startServer(MgmtdServer &server, const flat::AppInfo &appInfo);

 private:
  void init(const kv::HybridKvEngineConfig &config, bool useMemKV, const kv::fdb::FDBConfig &fdbConfig);

  std::shared_ptr<kv::IKVEngine> kvEngine_;
};
}  // namespace hf3fs::mgmtd
