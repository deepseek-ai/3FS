#include "MgmtdLauncherConfig.h"

#include "common/app/ApplicationBase.h"
#include "common/app/Utils.h"

namespace hf3fs::mgmtd {
void MgmtdLauncherConfig::init(const String &filePath, bool dump, const std::vector<config::KeyValue> &updates) {
  app_detail::initConfigFromFile(*this, filePath, dump, updates);

  auto rv = flat::ReleaseVersion::fromVersionInfo();
  if (!allow_dev_version() && !rv.getIsReleaseVersion()) {
    XLOGF(FATAL, "Dev version is not allowed: {}", rv);
  }
}
}  // namespace hf3fs::mgmtd
