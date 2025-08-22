#ifdef ENABLE_FUSE_APPLICATION

#include "FuseApplication.h"

int main(int argc, char *argv[]) {
  gflags::AllowCommandLineReparsing();
  using namespace hf3fs;
  return fuse::FuseApplication().run(argc, argv);
}
#else
#include <folly/ScopeGuard.h>

#include "FuseConfig.h"
#include "FuseMainLoop.h"
#include "FuseOps.h"
#include "common/logging/LogInit.h"

using namespace hf3fs;
using namespace hf3fs::fuse;

DECLARE_string(cfg);
DECLARE_bool(use_local_cfg);

auto withRetry(auto &&f, std::string_view desc) {
  using RetType = decltype(f());
  auto retryInterval = std::chrono::milliseconds(10);
  constexpr auto maxRetryInterval = std::chrono::milliseconds(1000);
  std::optional<RetType> res;
  for (int i = 0; i < 20; ++i) {
    res = f();
    if (*res) break;
    XLOGF(CRITICAL, "{} failed: {}\nretryCount: {}", desc, res->error(), i);
    std::this_thread::sleep_for(retryInterval);
    retryInterval = std::min(2 * retryInterval, maxRetryInterval);
  }
  return *res;
}

int main(int argc, char *argv[]) {
  gflags::AllowCommandLineReparsing();
  FuseConfig hf3fsConfig;
  hf3fsConfig.init(&argc, &argv);

  auto ibResult = net::IBManager::start(hf3fsConfig.ib_devices());
  XLOGF_IF(FATAL, !ibResult, "Failed to start IBManager: {}", ibResult.error());
  SCOPE_EXIT { hf3fs::net::IBManager::stop(); };

  auto logConfigStr = logging::generateLogConfig(hf3fsConfig.log(), String("hf3fs_fuse"));
  XLOGF(INFO, "LogConfig: {}", logConfigStr);
  logging::initOrDie(logConfigStr);
  XLOGF(INFO, "{}", VersionInfo::full());

  auto monitorResult = monitor::Monitor::start(hf3fsConfig.monitor());
  XLOGF_IF(FATAL, !monitorResult, "Parse config file from flags failed: {}", monitorResult.error());

  auto physicalHostnameRes = SysResource::hostname(/*physicalMachineName=*/true);
  XLOGF_IF(FATAL, !physicalHostnameRes, "Get physical hostname failed: {}", physicalHostnameRes.error());

  auto containerHostnameRes = SysResource::hostname(/*physicalMachineName=*/false);
  XLOGF_IF(FATAL, !containerHostnameRes, "Get container hostname failed: {}", containerHostnameRes.error());

  auto clientId = ClientId::random(*physicalHostnameRes);

  flat::AppInfo appInfo;
  appInfo.clusterId = hf3fsConfig.cluster_id();
  appInfo.hostname = *physicalHostnameRes;
  appInfo.pid = SysResource::pid();
  appInfo.releaseVersion = flat::ReleaseVersion::fromVersionInfo();

  auto &d = getFuseClientsInstance();
  if (auto res = d.init(appInfo, hf3fsConfig.mountpoint(), hf3fsConfig.token_file(), hf3fsConfig); !res) {
    XLOGF(FATAL, "Init fuse clients failed: {}", res.error());
  }
  SCOPE_EXIT { d.stop(); };

  return fuseMainLoop(argv[0],
                      hf3fsConfig.allow_other(),
                      hf3fsConfig.mountpoint(),
                      hf3fsConfig.io_bufs().max_buf_size(),
                      hf3fsConfig.cluster_id());
}

#endif
