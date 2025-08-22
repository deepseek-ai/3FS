#pragma once

#include "LauncherUtils.h"
#include "common/app/ApplicationBase.h"
#include "common/utils/ConstructLog.h"
#include "fbs/mgmtd/ConfigInfo.h"

DECLARE_string(app_cfg);
DECLARE_bool(dump_default_app_cfg);

DECLARE_string(launcher_cfg);
DECLARE_bool(dump_default_launcher_cfg);

namespace hf3fs::core {
class ServerLauncherBase {
 public:
  Result<Void> parseFlags(int *argc, char ***argv);

 protected:
  ApplicationBase::ConfigFlags appConfigFlags_;
  ApplicationBase::ConfigFlags launcherConfigFlags_;
};

template <typename Server>
class ServerLauncher : public ServerLauncherBase {
 public:
  using AppConfig = typename Server::AppConfig;
  using LauncherConfig = typename Server::LauncherConfig;
  using RemoteConfigFetcher = typename Server::RemoteConfigFetcher;
  static constexpr auto kNodeType = Server::kNodeType;

  ServerLauncher() = default;

  Result<Void> init() {
    appConfig_.init(FLAGS_app_cfg, FLAGS_dump_default_app_cfg, appConfigFlags_);
    launcherConfig_.init(FLAGS_launcher_cfg, FLAGS_dump_default_launcher_cfg, launcherConfigFlags_);

    XLOGF(INFO, "Full AppConfig:\n{}", appConfig_.toString());
    XLOGF(INFO, "Full LauncherConfig:\n{}", launcherConfig_.toString());

    auto ibResult = net::IBManager::start(launcherConfig_.ib_devices());
    XLOGF_IF(FATAL, !ibResult, "Failed to start IBManager: {}", ibResult.error());
    XLOGF(INFO, "IBDevice inited");

    fetcher_ = std::make_unique<RemoteConfigFetcher>(launcherConfig_);
    return Void{};
  }

  Result<std::pair<String, String>> loadConfigTemplate() {
    auto res = fetcher_->loadConfigTemplate(kNodeType);
    RETURN_ON_ERROR(res);
    return std::make_pair(res->content, res->genUpdateDesc());
  }

  Result<flat::AppInfo> loadAppInfo() {
    auto appInfo = launcher::buildBasicAppInfo(appConfig_.getNodeId(), launcherConfig_.cluster_id());
    RETURN_ON_ERROR(fetcher_->completeAppInfo(appInfo));
    return appInfo;
  }

  Result<Void> startServer(Server &server, const flat::AppInfo &appInfo) {
    if constexpr (requires { fetcher_->startServer(server, appInfo); }) {
      return fetcher_->startServer(server, appInfo);
    } else {
      return server.start(appInfo);
    }
  }

  const auto &appConfig() const { return appConfig_; }

  const auto &launcherConfig() const { return launcherConfig_; }

 private:
  AppConfig appConfig_;
  LauncherConfig launcherConfig_;

  std::unique_ptr<RemoteConfigFetcher> fetcher_;
};
}  // namespace hf3fs::core
