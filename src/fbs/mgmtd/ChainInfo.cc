#include "ChainInfo.h"

#include "common/serde/SerdeComparisons.h"

namespace hf3fs::flat {
bool ChainInfo::operator==(const ChainInfo &other) const { return serde::equals(*this, other); }
}  // namespace hf3fs::flat
