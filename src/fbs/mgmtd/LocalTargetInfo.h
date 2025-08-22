#pragma once

#include "MgmtdTypes.h"
#include "common/serde/SerdeComparisons.h"
#include "common/serde/SerdeHelper.h"

namespace hf3fs::flat {
struct LocalTargetInfo : public serde::SerdeHelper<LocalTargetInfo> {
  bool operator==(const LocalTargetInfo &other) const { return serde::equals(*this, other); }

  SERDE_STRUCT_FIELD(targetId, TargetId(0));
  SERDE_STRUCT_FIELD(localState, LocalTargetState(LocalTargetState::INVALID));
  SERDE_STRUCT_FIELD(diskIndex, std::optional<uint32_t>{});
  SERDE_STRUCT_FIELD(usedSize, uint64_t{});
  SERDE_STRUCT_FIELD(chainVersion, ChainVersion{});
  SERDE_STRUCT_FIELD(lowSpace, false);
};
}  // namespace hf3fs::flat
