#pragma once

#include "ChainTargetSetting.h"

namespace hf3fs::flat {
struct ChainSetting : public serde::SerdeHelper<ChainSetting> {
  SERDE_STRUCT_FIELD(chainId, ChainId(0));
  SERDE_STRUCT_FIELD(targets, std::vector<ChainTargetSetting>{});
  SERDE_STRUCT_FIELD(setPreferredTargetOrder, false);
};
}  // namespace hf3fs::flat
