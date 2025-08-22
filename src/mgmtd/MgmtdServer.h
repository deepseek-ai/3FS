#pragma once

#include "MgmtdConfigFetcher.h"
#include "MgmtdLauncherConfig.h"
#include "common/net/Server.h"
#include "core/app/ServerAppConfig.h"
#include "core/app/ServerLauncher.h"
#include "fdb/HybridKvEngineConfig.h"
#include "mgmtd/service/MgmtdOperator.h"
#include "service/MgmtdConfig.h"

namespace hf3fs::mgmtd {

class MgmtdServer : public net::Server {
 public:
  static constexpr auto kName = "Mgmtd";
  static constexpr auto kNodeType = flat::NodeType::MGMTD;

  using AppConfig = core::ServerAppConfig;
  using LauncherConfig = MgmtdLauncherConfig;
  using RemoteConfigFetcher = MgmtdConfigFetcher;
  using Launcher = core::ServerLauncher<MgmtdServer>;

  using CommonConfig = ApplicationBase::Config;
  class Config : public ConfigBase<Config> {
    CONFIG_OBJ(base, net::Server::Config, [](net::Server::Config &c) {
      c.set_groups_length(2);
      c.groups(0).listener().set_listen_port(8000);
      c.groups(0).set_services({"Mgmtd"});

      c.groups(1).set_network_type(net::Address::TCP);
      c.groups(1).listener().set_listen_port(9000);
      c.groups(1).set_use_independent_thread_pool(true);
      c.groups(1).set_services({"Core"});
    });
    CONFIG_OBJ(service, MgmtdConfig);
  };

  explicit MgmtdServer(const Config &config);
  ~MgmtdServer() override;

  using net::Server::start;
  Result<Void> start(const flat::AppInfo &info, std::shared_ptr<kv::IKVEngine> kvEngine);

  Result<Void> beforeStart() final;

  Result<Void> afterStop() final;

 private:
  const Config &config_;

  std::shared_ptr<kv::IKVEngine> kvEngine_;
  std::unique_ptr<MgmtdOperator> mgmtdOperator_;

  friend class testing::MgmtdTestHelper;
};

}  // namespace hf3fs::mgmtd
