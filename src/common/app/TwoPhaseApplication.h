#pragma once

#include <fmt/format.h>

#include "Utils.h"
#include "common/app/ApplicationBase.h"
#include "common/utils/LogCommands.h"

DECLARE_string(cfg);
DECLARE_bool(dump_default_cfg);
DECLARE_bool(use_local_cfg);

namespace hf3fs {
template <typename Server>
class TwoPhaseApplication : public ApplicationBase {
  using Launcher = typename Server::Launcher;
  using ServerConfig = typename Server::Config;

 public:
  TwoPhaseApplication()
      : launcher_(std::make_unique<Launcher>()) {}

 private:
  struct Config : public ConfigBase<Config> {
    CONFIG_OBJ(common, typename Server::CommonConfig);
    CONFIG_OBJ(server, ServerConfig);
  };

  Result<Void> parseFlags(int *argc, char ***argv) final {
    RETURN_ON_ERROR(launcher_->parseFlags(argc, argv));

    static constexpr std::string_view dynamicConfigPrefix = "--config.";
    return ApplicationBase::parseFlags(dynamicConfigPrefix, argc, argv, configFlags_);
  }

  Result<Void> initApplication() final {
    if (FLAGS_dump_default_cfg) {
      fmt::print("{}\n", config_.toString());
      exit(0);
    }

    auto firstInitRes = launcher_->init();
    XLOGF_IF(FATAL, !firstInitRes, "Failed to init launcher: {}", firstInitRes.error());

    app_detail::loadAppInfo([this] { return launcher_->loadAppInfo(); }, appInfo_);
    app_detail::initConfig(config_, configFlags_, appInfo_, [this] { return launcher_->loadConfigTemplate(); });
    XLOGF(INFO, "Server config inited");

    app_detail::initCommonComponents(config_.common(), Server::kName, appInfo_.nodeId);

    onLogConfigUpdated_ = app_detail::makeLogConfigUpdateCallback(config_.common().log(), Server::kName);
    onMemConfigUpdated_ = app_detail::makeMemConfigUpdateCallback(config_.common().memory(), appInfo_.hostname);

    XLOGF(INFO, "Full Config:\n{}", config_.toString());
    app_detail::persistConfig(config_);

    XLOGF(INFO, "Start to init server");
    auto initRes = initServer();
    XLOGF_IF(FATAL, !initRes, "Init server failed: {}", initRes.error());
    XLOGF(INFO, "Init server finished");

    XLOGF(INFO, "Start to start server");
    auto startRes = startServer();
    XLOGF_IF(FATAL, !startRes, "Start server failed: {}", startRes.error());
    XLOGF(INFO, "Start server finished");

    launcher_.reset();

    return Void{};
  }

  void stop() final {
    XLOGF(INFO, "Stop TwoPhaseApplication...");
    if (launcher_) {
      launcher_.reset();
    }
    stopAndJoin(server_.get());
    server_.reset();
    XLOGF(INFO, "Stop TwoPhaseApplication finished");
  }

  config::IConfig *getConfig() final { return &config_; }

  const flat::AppInfo *info() const final { return &appInfo_; }

  bool configPushable() const final { return FLAGS_cfg.empty() && !FLAGS_use_local_cfg; }

  void onConfigUpdated() { app_detail::persistConfig(config_); }

 private:
  Result<Void> initServer() {
    server_ = std::make_unique<Server>(config_.server());
    RETURN_ON_ERROR(server_->setup());
    XLOGF(INFO, "{}", server_->describe());
    return Void{};
  }

  Result<Void> startServer() {
    auto startResult = launcher_->startServer(*server_, appInfo_);
    XLOGF_IF(FATAL, !startResult, "Start server failed: {}", startResult.error());
    appInfo_ = server_->appInfo();
    return Void{};
  }

  ConfigFlags configFlags_;

  Config config_;
  flat::AppInfo appInfo_;
  std::unique_ptr<Launcher> launcher_;
  std::unique_ptr<Server> server_;
  std::unique_ptr<ConfigCallbackGuard> onLogConfigUpdated_;
  std::unique_ptr<ConfigCallbackGuard> onMemConfigUpdated_;
};
}  // namespace hf3fs
