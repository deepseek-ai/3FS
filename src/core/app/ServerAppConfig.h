#pragma once

#include "common/app/NodeId.h"
#include "common/net/ib/IBDevice.h"
#include "common/utils/ConfigBase.h"

namespace hf3fs::core {
struct ServerAppConfig : public ConfigBase<ServerAppConfig> {
  CONFIG_ITEM(node_id, 0);
  CONFIG_ITEM(allow_empty_node_id, true);

 public:
  using Base = ConfigBase<ServerAppConfig>;
  using Base::init;

  void init(const String &filePath, bool dump, const std::vector<config::KeyValue> &updates);

  flat::NodeId getNodeId() const { return flat::NodeId(node_id()); }
};
}  // namespace hf3fs::core
