#pragma once

#include "ChainTargetInfo.h"

namespace hf3fs::flat {
struct ChainInfo : public serde::SerdeHelper<ChainInfo> {
  SERDE_STRUCT_FIELD(chainId, ChainId(0));
  SERDE_STRUCT_FIELD(chainVersion, ChainVersion(0));
  SERDE_STRUCT_FIELD(targets, std::vector<ChainTargetInfo>{});
  SERDE_STRUCT_FIELD(preferredTargetOrder, std::vector<TargetId>{});

  bool operator==(const ChainInfo &other) const;
};
}  // namespace hf3fs::flat
