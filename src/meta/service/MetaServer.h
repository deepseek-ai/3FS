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
#include "fdb/HybridKvEngineConfig.h"
#include "meta/base/Config.h"
#include "meta/service/MetaOperator.h"

namespace hf3fs::meta::server {

class MetaServer : public net::Server {
 public:
  static constexpr auto kName = "Meta";
  static constexpr auto kNodeType = flat::NodeType::META;

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
  using Launcher = core::ServerLauncher<MetaServer>;

  struct Config : public ConfigBase<Config> {
    CONFIG_ITEM(use_memkv, false);  // deprecated

    CONFIG_OBJ(base, net::Server::Config, [](net::Server::Config &c) {
      c.set_groups_length(2);
      c.groups(0).listener().set_listen_port(8000);
      c.groups(0).set_services({"MetaSerde"});

      c.groups(1).set_network_type(net::Address::TCP);
      c.groups(1).listener().set_listen_port(9000);
      c.groups(1).set_use_independent_thread_pool(true);
      c.groups(1).set_services({"Core"});
    });
    CONFIG_OBJ(fdb, kv::fdb::FDBConfig);  // deprecated
    CONFIG_OBJ(meta, meta::server::Config);
    CONFIG_OBJ(background_client, net::Client::Config);
    CONFIG_OBJ(mgmtd_client, ::hf3fs::client::MgmtdClientForServer::Config);
    CONFIG_OBJ(storage_client, storage::client::StorageClient::Config, [](storage::client::StorageClient::Config &cfg) {
      cfg.retry().set_init_wait_time(2_s);
      cfg.retry().set_max_wait_time(5_s);
      cfg.retry().set_max_retry_time(5_s);
      cfg.retry().set_max_failures_before_failover(1);
    });
    CONFIG_OBJ(kv_engine, kv::HybridKvEngineConfig);
  };

  MetaServer(const Config &config);
  ~MetaServer() override;

  using net::Server::start;
  Result<Void> start(const flat::AppInfo &info, std::shared_ptr<kv::IKVEngine> kvEngine);
  Result<Void> start(const flat::AppInfo &info,
                     std::unique_ptr<net::Client> client,
                     std::shared_ptr<::hf3fs::client::MgmtdClient> mgmtdClient);

  // set up meta server.
  Result<Void> beforeStart() final;

  // tear down meta server.
  Result<Void> beforeStop() final;
  Result<Void> afterStop() final;

 private:
  const Config &config_;

  std::shared_ptr<kv::IKVEngine> kvEngine_;
  std::unique_ptr<net::Client> backgroundClient_;
  std::shared_ptr<::hf3fs::client::MgmtdClientForServer> mgmtdClient_;
  std::unique_ptr<MetaOperator> metaOperator_;
};

}  // namespace hf3fs::meta::server
