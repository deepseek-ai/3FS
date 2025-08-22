#pragma once

#include "fbs/mgmtd/LocalTargetInfo.h"

namespace hf3fs::mgmtd {
struct LocalTargetInfoWithNodeId {
  flat::TargetId targetId{0};
  flat::NodeId nodeId{0};
  flat::LocalTargetState localState{flat::LocalTargetState::INVALID};

  LocalTargetInfoWithNodeId() = default;
  LocalTargetInfoWithNodeId(flat::NodeId nodeId, flat::LocalTargetInfo info)
      : targetId(info.targetId),
        nodeId(nodeId),
        localState(info.localState) {}
  LocalTargetInfoWithNodeId(flat::TargetId i, flat::NodeId ni, flat::LocalTargetState state)
      : targetId(i),
        nodeId(ni),
        localState(state) {}
};
}  // namespace hf3fs::mgmtd
