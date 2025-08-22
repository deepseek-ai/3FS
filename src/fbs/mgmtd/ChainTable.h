#pragma once

#include "ChainInfo.h"
#include "common/serde/SerdeComparisons.h"

namespace hf3fs::flat {
struct ChainTable : public serde::SerdeHelper<ChainTable> {
  bool operator==(const ChainTable &other) const { return serde::equals(*this, other); }

  SERDE_STRUCT_FIELD(chainTableId, ChainTableId(0));
  SERDE_STRUCT_FIELD(chainTableVersion, ChainTableVersion(0));
  SERDE_STRUCT_FIELD(chains, std::vector<ChainId>{});
  SERDE_STRUCT_FIELD(desc, String{});
};
}  // namespace hf3fs::flat
