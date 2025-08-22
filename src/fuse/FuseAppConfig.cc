#include "FuseAppConfig.h"

#include "common/app/ApplicationBase.h"

namespace hf3fs::fuse {
void FuseAppConfig::init(const String &filePath, bool dump, const std::vector<config::KeyValue> &updates) {
  auto res = ApplicationBase::initConfig(*this, filePath, dump, updates);
  XLOGF_IF(FATAL, !res, "Init app config failed: {}. filePath: {}. dump: {}", res.error(), filePath, dump);
}
}  // namespace hf3fs::fuse
