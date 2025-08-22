#pragma once

#include "MgmtdTypes.h"
#include "common/serde/SerdeComparisons.h"
#include "common/serde/SerdeHelper.h"

namespace hf3fs::flat {
struct ChainTargetInfo : public serde::SerdeHelper<ChainTargetInfo> {
  bool operator==(const ChainTargetInfo &other) const { return serde::equals(*this, other); }

  SERDE_STRUCT_FIELD(targetId, TargetId(0));
  SERDE_STRUCT_FIELD(publicState, PublicTargetState(PublicTargetState::INVALID));
};
}  // namespace hf3fs::flat
