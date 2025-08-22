#pragma once

#include <folly/CancellationToken.h>

#include "client/mgmtd/MgmtdClientForServer.h"
#include "common/net/Server.h"
#include "core/app/ServerAppConfig.h"
#include "core/app/ServerLauncher.h"
#include "core/app/ServerLauncherConfig.h"
#include "core/app/ServerMgmtdClientFetcher.h"
#include "storage/service/Components.h"
#include "storage/service/ReliableForwarding.h"
#include "storage/service/ReliableUpdate.h"
#include "storage/service/StorageOperator.h"

namespace hf3fs::test {
struct StorageServerHelper;
}

namespace hf3fs::storage {

class StorageServer : public net::Server {
 public:
  static constexpr auto kName = "Storage";
  static constexpr auto kNodeType = flat::NodeType::STORAGE;

  using AppConfig = core::ServerAppConfig;
  struct LauncherConfig : public core::ServerLauncherConfig {
    LauncherConfig() { mgmtd_client() = hf3fs::client::MgmtdClientForServer::Config{}; }
  };
  using RemoteConfigFetcher = core::launcher::ServerMgmtdClientFetcher;
  using Launcher = core::ServerLauncher<StorageServer>;

  using CommonConfig = ApplicationBase::Config;
  using Config = Components::Config;
  StorageServer(const Components::Config &config);
  ~StorageServer() override;

  // set up storage server.
  Result<Void> beforeStart() final;

  // before server stop.
  Result<Void> beforeStop() final;

  // tear down storage server.
  Result<Void> afterStop() final;

  using net::Server::start;
  hf3fs::Result<Void> start(const flat::AppInfo &info,
                            std::unique_ptr<::hf3fs::net::Client> client,
                            std::shared_ptr<::hf3fs::client::MgmtdClient> mgmtdClient);

 private:
  friend struct test::StorageServerHelper;
  ConstructLog<"storage::StorageServer"> constructLog_;
  Components components_;
};

}  // namespace hf3fs::storage
