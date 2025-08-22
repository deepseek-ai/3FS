#ifdef ENABLE_FUSE_APPLICATION

#include "FuseApplication.h"

#include "FuseMainLoop.h"
#include "FuseOps.h"
#include "common/app/Thread.h"
#include "common/app/Utils.h"

DECLARE_string(cfg);
DECLARE_bool(dump_default_cfg);
DECLARE_bool(use_local_cfg);

namespace hf3fs::fuse {

struct FuseApplication::Impl {
  Result<Void> parseFlags(int *argc, char ***argv);
  Result<Void> initApplication();
  Result<Void> initFuseClients();
  void stop();
  int mainLoop();

  Config hf3fsConfig;
  flat::AppInfo appInfo;
  std::unique_ptr<Launcher> launcher_ = std::make_unique<Launcher>();

  std::unique_ptr<ConfigCallbackGuard> onLogConfigUpdated_;
  std::unique_ptr<ConfigCallbackGuard> onMemConfigUpdated_;

  ConfigFlags configFlags_;
  String programName;
  bool allowOther = false;
  String configMountpoint;
  size_t configMaxBufSize = 0;
  String configClusterId;
};

FuseApplication::FuseApplication()
    : impl_(std::make_unique<Impl>()) {}

FuseApplication::~FuseApplication() = default;

Result<Void> FuseApplication::Impl::parseFlags(int *argc, char ***argv) {
  RETURN_ON_ERROR(launcher_->parseFlags(argc, argv));

  static constexpr std::string_view dynamicConfigPrefix = "--config.";
  RETURN_ON_ERROR(ApplicationBase::parseFlags(dynamicConfigPrefix, argc, argv, configFlags_));

  programName = (*argv)[0];
  return Void{};
}

Result<Void> FuseApplication::parseFlags(int *argc, char ***argv) { return impl_->parseFlags(argc, argv); }

Result<Void> FuseApplication::Impl::initApplication() {
  if (FLAGS_dump_default_cfg) {
    fmt::print("{}\n", hf3fsConfig.toString());
    exit(0);
  }

  auto firstInitRes = launcher_->init();
  XLOGF_IF(FATAL, !firstInitRes, "Failed to init launcher: {}", firstInitRes.error());

  app_detail::loadAppInfo([this] { return launcher_->loadAppInfo(); }, appInfo);
  app_detail::initConfig(hf3fsConfig, configFlags_, appInfo, [this] { return launcher_->loadConfigTemplate(); });
  XLOGF(INFO, "Server config inited");

  app_detail::initCommonComponents(hf3fsConfig.common(), kName, appInfo.nodeId);

  onLogConfigUpdated_ = app_detail::makeLogConfigUpdateCallback(hf3fsConfig.common().log(), kName);
  onMemConfigUpdated_ = app_detail::makeMemConfigUpdateCallback(hf3fsConfig.common().memory(), appInfo.hostname);

  XLOGF(INFO, "Full Config:\n{}", hf3fsConfig.toString());
  app_detail::persistConfig(hf3fsConfig);

  XLOGF(INFO, "Start to init fuse clients");
  auto initRes = initFuseClients();
  XLOGF_IF(FATAL, !initRes, "Init fuse clients failed: {}", initRes.error());
  XLOGF(INFO, "Init fuse clients finished");

  launcher_.reset();

  return Void{};
}

Result<Void> FuseApplication::Impl::initFuseClients() {
  const auto &launcherConfig = launcher_->launcherConfig();
  allowOther = launcherConfig.allow_other();
  configMountpoint = launcherConfig.mountpoint();
  configMaxBufSize = hf3fsConfig.io_bufs().max_buf_size();
  configClusterId = launcherConfig.cluster_id();

  auto &d = getFuseClientsInstance();
  RETURN_ON_ERROR(d.init(appInfo, launcherConfig.mountpoint(), launcherConfig.token_file(), hf3fsConfig));
  return Void{};
}

Result<Void> FuseApplication::initApplication() { return impl_->initApplication(); }

void FuseApplication::Impl::stop() {
  getFuseClientsInstance().stop();
  hf3fs::stopAndJoin(nullptr);
}

void FuseApplication::stop() { impl_->stop(); }

config::IConfig *FuseApplication::getConfig() { return &impl_->hf3fsConfig; }

const flat::AppInfo *FuseApplication::info() const { return &impl_->appInfo; }

bool FuseApplication::configPushable() const { return FLAGS_cfg.empty() && !FLAGS_use_local_cfg; }

void FuseApplication::onConfigUpdated() { app_detail::persistConfig(impl_->hf3fsConfig); }

int FuseApplication::Impl::mainLoop() {
  Thread::unblockInterruptSignals();

  return fuseMainLoop(programName, allowOther, configMountpoint, configMaxBufSize, configClusterId);
}

int FuseApplication::mainLoop() { return impl_->mainLoop(); }

}  // namespace hf3fs::fuse

#endif
