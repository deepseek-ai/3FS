#include "fbs/storage/Common.h"

#include <fmt/format.h>

#include "common/utils/Result.h"
#include "scn/tuple_return/tuple_return.h"

namespace hf3fs::storage {

ChunkId::ChunkId(uint64_t high, uint64_t low) {
  data_.resize(sizeof(high) + sizeof(low));
  if constexpr (std::endian::native == std::endian::little) {
    high = __builtin_bswap64(high);
    low = __builtin_bswap64(low);
  }
  *(reinterpret_cast<uint64_t *>(&data_[0])) = high;
  *(reinterpret_cast<uint64_t *>(&data_[sizeof(high)])) = low;
}

ChunkId::ChunkId(const ChunkId &baseChunkId, uint64_t chunkIndex)
    : data_(baseChunkId.data_) {
  if (data_.size() < sizeof(uint64_t)) {
    data_.insert(data_.begin(), sizeof(uint64_t) - data_.size(), 0);
    return;
  }

  auto lowPtr = reinterpret_cast<uint64_t *>(&data_[data_.size() - sizeof(uint64_t)]);

  if constexpr (std::endian::native == std::endian::little) {
    *lowPtr = __builtin_bswap64(__builtin_bswap64(*lowPtr) + chunkIndex);
  } else {
    *lowPtr = *lowPtr + chunkIndex;
  }
}

ChunkId ChunkId::nextChunkId() const {
  auto data = data_;
  for (auto it = data.rbegin(); it != data.rend(); ++it) {
    if (++*it != 0) {
      break;
    }
  }
  return ChunkId{std::move(data)};
}

ChunkId ChunkId::rangeEndForCurrentChunk() const {
  std::string data;
  data.reserve(data.size() + 1);
  data.append(data_);
  data.push_back(0);
  return ChunkId{std::move(data)};
}

std::string ChunkId::describe() const {
  std::string ret;
  ret.reserve(36);
  for (uint32_t i = 0; i < data_.size(); ++i) {
    if (i && i % 4 == 0) {
      ret += '-';
    }
    fmt::format_to(std::back_inserter(ret), "{:02X}", uint8_t(data_[i]));
  }
  return ret;
}

Result<ChunkId> ChunkId::fromString(std::string_view str) {
  if (str.empty()) {
    return ChunkId{};
  }
  ChunkId out;
  std::optional<uint8_t> half;
  for (auto ch : str) {
    uint8_t value = 0;
    if ('0' <= ch && ch <= '9') {
      value = ch - '0';
    } else if ('A' <= ch && ch <= 'F') {
      value = ch - 'A' + 10;
    } else if ('a' <= ch && ch <= 'f') {
      value = ch - 'a' + 10;
    } else if (ch == '-') {
      continue;
    } else {
      return makeError(StatusCode::kInvalidFormat, fmt::format("invalid chunk id: {}", str));
    }
    if (half) {
      out.data_.push_back(*half * 16u + value);
      half = std::nullopt;
    } else {
      half = value;
    }
  }
  if (out.data_.length() != 16u) {
    return makeError(StatusCode::kInvalidFormat, fmt::format("invalid chunk id: {}, {}", str, out.describe()));
  }
  return out;
}

GlobalKey GlobalKey::fromFileOffset(const std::vector<VersionedChainId> &chainIds,
                                    const ChunkId &baseChunkId,
                                    const size_t chunkSize,
                                    const size_t fileOffset) {
  size_t chunkIndex = fileOffset / chunkSize;
  if (chunkIndex > UINT32_MAX) return {};

  ChunkId chunkId(baseChunkId, chunkIndex);
  return {chainIds[chunkIndex], chunkId};
}

}  // namespace hf3fs::storage
