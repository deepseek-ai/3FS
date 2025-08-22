#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <folly/Hash.h>
#include <folly/lang/Bits.h>
#include <string>

#include "common/serde/BigEndian.h"
#include "common/serde/Serde.h"
#include "common/utils/Int128.h"
#include "common/utils/Result.h"
#include "common/utils/Size.h"
#include "common/utils/StrongType.h"
#include "fbs/storage/Common.h"
#include "storage/store/ChunkFileView.h"

namespace hf3fs::storage {

inline constexpr auto kMaxChunkSize = 64_MB;

struct ChunkInfo {
  ChunkMetadata meta;
  ChunkFileView view;
};

struct ChunkPosition {
  SERDE_STRUCT_FIELD(fileIdx, uint32_t{});
  SERDE_STRUCT_FIELD(offset, serde::BigEndian<std::size_t>{});
};

void reportFatalEvent();

}  // namespace hf3fs::storage

template <>
struct ::std::hash<hf3fs::storage::ChunkFileId> {
  size_t operator()(hf3fs::storage::ChunkFileId id) const {
    return folly::hash::twang_mix64(reinterpret_cast<uint64_t &>(id));
  }
};
