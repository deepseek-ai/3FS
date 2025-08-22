#pragma once

#include "MgmtdTypes.h"
#include "common/serde/SerdeComparisons.h"
#include "common/serde/SerdeHelper.h"

namespace hf3fs::flat {
struct ChainTargetSetting : public serde::SerdeHelper<ChainTargetSetting> {
  SERDE_STRUCT_FIELD(targetId, TargetId(0));

  auto operator<=>(const ChainTargetSetting &other) const { return serde::compare(*this, other); }
};
}  // namespace hf3fs::flat
