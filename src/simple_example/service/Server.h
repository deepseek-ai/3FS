#pragma once

#include <memory>

#include "client/mgmtd/MgmtdClientForServer.h"
#include "client/storage/StorageClient.h"
#include "common/logging/LogConfig.h"
#include "common/net/Client.h"
#include "common/net/Server.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/ConfigBase.h"
#include "core/app/ServerAppConfig.h"
#include "core/app/ServerLauncher.h"
#include "core/app/ServerLauncherConfig.h"
#include "core/app/ServerMgmtdClientFetcher.h"

namespace hf3fs::simple_example::server {

class SimpleExampleServer : public net::Server {
 public:
  static constexpr auto kName = "SimpleExample";
  static constexpr auto kNodeType = flat::NodeType::CLIENT;

  struct CommonConfig : public ApplicationBase::Config {
    CommonConfig() {
      using logging::LogConfig;
      log().set_categories({LogConfig::makeRootCategoryConfig(), LogConfig::makeEventCategoryConfig()});
      log().set_handlers({LogConfig::makeNormalHandlerConfig(),
                          LogConfig::makeErrHandlerConfig(),
                          LogConfig::makeFatalHandlerConfig(),
                          LogConfig::makeEventHandlerConfig()});
    }
  };

  using AppConfig = core::ServerAppConfig;
  struct LauncherConfig : public core::ServerLauncherConfig {
    LauncherConfig() { mgmtd_client() = hf3fs::client::MgmtdClientForServer::Config{}; }
  };
  using RemoteConfigFetcher = core::launcher::ServerMgmtdClientFetcher;
  using Launcher = core::ServerLauncher<SimpleExampleServer>;

  struct Config : public ConfigBase<Config> {
    CONFIG_OBJ(base, net::Server::Config, [](net::Server::Config &c) {
      c.set_groups_length(2);
      c.groups(0).listener().set_listen_port(8000);
      c.groups(0).set_services({"SimpleExampleSerde"});

      c.groups(1).set_network_type(net::Address::TCP);
      c.groups(1).listener().set_listen_port(9000);
      c.groups(1).set_use_independent_thread_pool(true);
      c.groups(1).set_services({"Core"});
    });
    CONFIG_OBJ(background_client, net::Client::Config);
    CONFIG_OBJ(mgmtd_client, ::hf3fs::client::MgmtdClientForServer::Config);
    CONFIG_OBJ(storage_client, storage::client::StorageClient::Config);
  };

  SimpleExampleServer(const Config &config);
  ~SimpleExampleServer() override;

  Result<Void> beforeStart() final;

  Result<Void> beforeStop() final;

 private:
  const Config &config_;

  std::unique_ptr<net::Client> backgroundClient_;
  std::shared_ptr<::hf3fs::client::MgmtdClientForServer> mgmtdClient_;
};

}  // namespace hf3fs::simple_example::server
