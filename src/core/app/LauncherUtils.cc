#include "LauncherUtils.h"

#include <folly/experimental/coro/BlockingWait.h>

#include "common/utils/SysResource.h"
#include "common/utils/VersionInfo.h"

namespace hf3fs::core::launcher {
flat::AppInfo buildBasicAppInfo(flat::NodeId nodeId, const String &clusterId) {
  auto hostnameResult = SysResource::hostname();
  XLOGF_IF(FATAL, !hostnameResult, "Get hostname failed: {}", hostnameResult.error());

  flat::AppInfo appInfo;
  appInfo.nodeId = nodeId;
  appInfo.clusterId = clusterId;
  appInfo.hostname = *hostnameResult;
  appInfo.pid = SysResource::pid();
  appInfo.releaseVersion = flat::ReleaseVersion::fromVersionInfo();
  return appInfo;
}
}  // namespace hf3fs::core::launcher
