#pragma once

#include "ChainTargetInfo.h"
#include "LocalTargetInfo.h"
#include "common/serde/SerdeComparisons.h"

namespace hf3fs::flat {
struct TargetInfo : public serde::SerdeHelper<TargetInfo> {
  bool operator==(const TargetInfo &other) const { return serde::equals(*this, other); }

  SERDE_STRUCT_FIELD(targetId, TargetId(0));
  SERDE_STRUCT_FIELD(publicState, PublicTargetState(PublicTargetState::INVALID));
  SERDE_STRUCT_FIELD(localState, LocalTargetState(LocalTargetState::INVALID));
  SERDE_STRUCT_FIELD(chainId, ChainId(0));
  SERDE_STRUCT_FIELD(nodeId, std::optional<NodeId>{});
  SERDE_STRUCT_FIELD(diskIndex, std::optional<uint32_t>{});
  SERDE_STRUCT_FIELD(usedSize, uint64_t{});
};
}  // namespace hf3fs::flat
