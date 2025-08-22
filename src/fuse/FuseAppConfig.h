#pragma once

#include "common/app/NodeId.h"
#include "common/net/ib/IBDevice.h"
#include "common/utils/ConfigBase.h"

namespace hf3fs::fuse {
struct FuseAppConfig : public ConfigBase<FuseAppConfig> {
 public:
  using Base = ConfigBase<FuseAppConfig>;
  using Base::init;

  void init(const String &filePath, bool dump, const std::vector<config::KeyValue> &updates);
  flat::NodeId getNodeId() const { return flat::NodeId(0); }
};
}  // namespace hf3fs::fuse
