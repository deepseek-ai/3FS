#include "FuseLauncherConfig.h"

#include "common/app/ApplicationBase.h"
#include "common/app/Utils.h"

namespace hf3fs::fuse {
void FuseLauncherConfig::init(const String &filePath, bool dump, const std::vector<config::KeyValue> &updates) {
  app_detail::initConfigFromFile(*this, filePath, dump, updates);
}
}  // namespace hf3fs::fuse
