#pragma once

#ifdef ENABLE_FUSE_APPLICATION

#include "FuseAppConfig.h"
#include "FuseConfig.h"
#include "FuseConfigFetcher.h"
#include "FuseLauncherConfig.h"
#include "common/app/ApplicationBase.h"
#include "core/app/ServerLauncher.h"

namespace hf3fs::fuse {
class FuseApplication : public ApplicationBase {
 public:
  static constexpr auto kName = "Fuse";
  static constexpr auto kNodeType = flat::NodeType::FUSE;

  using AppConfig = FuseAppConfig;
  using LauncherConfig = FuseLauncherConfig;
  using RemoteConfigFetcher = FuseConfigFetcher;
  using Launcher = core::ServerLauncher<FuseApplication>;

  using Config = FuseConfig;

  FuseApplication();
  ~FuseApplication();

 private:
  Result<Void> parseFlags(int *argc, char ***argv) final;

  Result<Void> initApplication() final;

  void stop() final;

  int mainLoop() final;

  config::IConfig *getConfig() final;

  const flat::AppInfo *info() const final;

  bool configPushable() const final;

  void onConfigUpdated() final;

 private:
  Result<Void> initServer();

  Result<Void> startServer();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace hf3fs::fuse

#endif
