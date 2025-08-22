#include "ServerAppConfig.h"

#include "common/app/ApplicationBase.h"

namespace hf3fs::core {
void ServerAppConfig::init(const String &filePath, bool dump, const std::vector<config::KeyValue> &updates) {
  auto res = ApplicationBase::initConfig(*this, filePath, dump, updates);
  XLOGF_IF(FATAL, !res, "Init app config failed: {}. filePath: {}. dump: {}", res.error(), filePath, dump);

  XLOGF_IF(FATAL, !allow_empty_node_id() && node_id() == 0, "node_id is not allowed to be 0");
}
}  // namespace hf3fs::core
