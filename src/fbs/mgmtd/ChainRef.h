#pragma once

#include <fmt/core.h>
#include <functional>

#include "MgmtdTypes.h"
#include "common/utils/StrongType.h"

namespace hf3fs::flat {

class ChainRef {
 public:
  ChainRef() {}
  ChainRef(ChainTableId tableId, ChainTableVersion tableVer, uint64_t index)
      : chainTableId(tableId),
        chainTableVersion(tableVer),
        chainIndex(index) {}

  std::tuple<ChainTableId, ChainTableVersion, uint64_t> decode() const {
    return std::make_tuple(chainTableId, chainTableVersion, chainIndex);
  }

  ChainTableId tableId() const { return chainTableId; }

  ChainTableVersion tableVersion() const { return chainTableVersion; }

  uint64_t index() const { return chainIndex; }

  auto operator<=>(const ChainRef &) const = default;

 private:
  ChainTableId chainTableId{0};
  ChainTableVersion chainTableVersion{0};
  uint64_t chainIndex{0};
};
}  // namespace hf3fs::flat

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::flat::ChainRef> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::flat::ChainRef &ref, FormatContext &ctx) const {
    auto [id, v, i] = ref.decode();
    return fmt::format_to(ctx.out(), "ChainRef({}@{}-{})", id.toUnderType(), v.toUnderType(), i);
  }
};

FMT_END_NAMESPACE
