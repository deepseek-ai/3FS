#pragma once

#include "MgmtdTypes.h"
#include "common/app/AppInfo.h"
#include "common/serde/SerdeComparisons.h"
#include "common/serde/SerdeHelper.h"

namespace hf3fs::flat {
struct PersistentNodeInfo : public serde::SerdeHelper<PersistentNodeInfo> {
  bool operator==(const PersistentNodeInfo &other) const { return serde::equals(*this, other); }

  SERDE_STRUCT_FIELD(nodeId, NodeId(0));
  SERDE_STRUCT_FIELD(type, NodeType(NodeType::MIN));
  SERDE_STRUCT_FIELD(serviceGroups, std::vector<ServiceGroupInfo>{});
  SERDE_STRUCT_FIELD(tags, std::vector<TagPair>{});
  SERDE_STRUCT_FIELD(hostname, String{});
};
}  // namespace hf3fs::flat
