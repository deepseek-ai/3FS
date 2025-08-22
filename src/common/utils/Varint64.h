#pragma once

#include <cstdint>
#include <fmt/format.h>

namespace hf3fs {

struct Varint64 {
  uint64_t value;

  Varint64() = default;
  Varint64(uint64_t value)
      : value(value) {}

  operator uint64_t &() { return value; }
  operator uint64_t() const { return value; }
};

}  // namespace hf3fs

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::Varint64> : formatter<uint64_t> {
  template <typename FormatContext>
  auto format(hf3fs::Varint64 val, FormatContext &ctx) const {
    return formatter<uint64_t>::format(static_cast<uint64_t>(val), ctx);
  }
};

FMT_END_NAMESPACE
