#pragma once

#include "MgmtdTypes.h"
#include "common/serde/SerdeHelper.h"
#include "common/utils/UtcTimeSerde.h"

namespace hf3fs::flat {
struct NodeStatusChange : public serde::SerdeHelper<NodeStatusChange> {
  SERDE_STRUCT_FIELD(status, NodeStatus(NodeStatus::MIN));
  SERDE_STRUCT_FIELD(time, UtcTime{});
};
}  // namespace hf3fs::flat
