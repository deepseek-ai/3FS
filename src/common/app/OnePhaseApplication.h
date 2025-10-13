#pragma once

#include <iostream>

#include "AppInfo.h"
#include "ApplicationBase.h"
#include "Utils.h"
#include "common/logging/LogInit.h"
#include "common/net/Server.h"
#include "common/net/ib/IBDevice.h"
#include "common/utils/LogCommands.h"
#include "common/utils/SysResource.h"

DECLARE_string(app_cfg);
DECLARE_bool(dump_default_app_cfg);

DECLARE_string(cfg);
DECLARE_bool(dump_cfg);
DECLARE_bool(dump_default_cfg);

namespace hf3fs {
template <class T>
requires requires {
  typename T::Config;
  std::string_view(T::kName);
}
class OnePhaseApplication : public ApplicationBase {
 public:
  class CommonConfig : public ConfigBase<CommonConfig> {
    CONFIG_ITEM(cluster_id, "");
    CONFIG_OBJ(log, logging::LogConfig);
    CONFIG_OBJ(monitor, monitor::Monitor::Config);
    CONFIG_OBJ(ib_devices, net::IBDevice::Config);
  };

  class Config : public ConfigBase<Config> {
   public:
    CONFIG_OBJ(common, CommonConfig);
    CONFIG_OBJ(server, typename T::Config);
  };

  struct AppConfig : public ConfigBase<AppConfig> {
    CONFIG_ITEM(node_id, 0);
    CONFIG_ITEM(allow_empty_node_id, true);
  };

  OnePhaseApplication(AppConfig &appConfig, Config &config)
      : appConfig_(appConfig),
        config_(config) {}

  static OnePhaseApplication &instance() {
    static AppConfig appConfig;
    static Config config;
    static OnePhaseApplication app(appConfig, config);
    return app;
  }

  Result<Void> parseFlags(int *argc, char ***argv) final {
    static constexpr std::string_view appConfigPrefix = "--app_config.";
    static constexpr std::string_view configPrefix = "--config.";

    RETURN_ON_ERROR(ApplicationBase::parseFlags(appConfigPrefix, argc, argv, appConfigFlags_));
    RETURN_ON_ERROR(ApplicationBase::parseFlags(configPrefix, argc, argv, configFlags_));
    return Void{};
  }

  Result<Void> initApplication() final {
    if (!FLAGS_app_cfg.empty()) {
      app_detail::initConfigFromFile(appConfig_, FLAGS_app_cfg, FLAGS_dump_default_app_cfg, appConfigFlags_);
    }
    XLOGF_IF(FATAL, !appConfig_.allow_empty_node_id() && appConfig_.node_id() == 0, "node_id is not allowed to be 0");

    if (!FLAGS_cfg.empty()) {
      app_detail::initConfigFromFile(config_, FLAGS_cfg, FLAGS_dump_default_cfg, configFlags_);
    }

    if (FLAGS_dump_cfg) {
      std::cout << config_.toString() << std::endl;
      exit(0);
    }

    // init basic components
    auto ibResult = net::IBManager::start(config_.common().ib_devices());
    XLOGF_IF(FATAL, !ibResult, "Failed to start IBManager: {}", ibResult.error());

    auto logConfigStr = logging::generateLogConfig(config_.common().log(), String(T::kName));
    XLOGF(INFO, "LogConfig: {}", logConfigStr);
    logging::initOrDie(logConfigStr);
    XLOGF(INFO, "{}", VersionInfo::full());
    XLOGF(INFO, "Full AppConfig:\n{}", appConfig_.toString());
    XLOGF(INFO, "Full Config:\n{}", config_.toString());

    XLOGF(INFO, "Init waiter singleton {}", fmt::ptr(&net::Waiter::instance()));

    auto monitorResult = monitor::Monitor::start(config_.common().monitor());
    XLOGF_IF(FATAL, !monitorResult, "Parse config file from flags failed: {}", monitorResult.error());

    // init server and node info.
    server_ = std::make_unique<T>(config_.server());
    auto setupResult = server_->setup();
    XLOGF_IF(FATAL, !setupResult, "Setup server failed: {}", setupResult.error());

    auto hostnameResult = SysResource::hostname(/*physicalMachineName=*/true);
    XLOGF_IF(FATAL, !hostnameResult, "Get hostname failed: {}", hostnameResult.error());

    auto podnameResult = SysResource::hostname(/*physicalMachineName=*/false);
    XLOGF_IF(FATAL, !podnameResult, "Get podname failed: {}", podnameResult.error());

    info_.nodeId = flat::NodeId(appConfig_.node_id());
    info_.clusterId = config_.common().cluster_id();
    info_.hostname = *hostnameResult;
    info_.podname = *podnameResult;
    info_.pid = SysResource::pid();
    info_.releaseVersion = flat::ReleaseVersion::fromVersionInfo();
    for (auto &group : server_->groups()) {
      info_.serviceGroups.emplace_back(group->serviceNameList(), group->addressList());
    }
    XLOGF(INFO, "{}", server_->describe());

    // 5. start server.
    auto startResult = server_->start(info_);
    XLOGF_IF(FATAL, !startResult, "Start server failed: {}", startResult.error());

    return Void{};
  }

  config::IConfig *getConfig() final { return &config_; }

  const flat::AppInfo *info() const final { return &info_; }

  void stop() final { stopAndJoin(server_.get()); }

 private:
  OnePhaseApplication() = default;

  ConfigFlags appConfigFlags_;
  ConfigFlags configFlags_;

  AppConfig &appConfig_;
  Config &config_;
  flat::AppInfo info_;
  std::unique_ptr<net::Server> server_;
};

}  // namespace hf3fs
