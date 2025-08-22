#pragma once

#include <cstdint>
#include <fmt/format.h>

namespace hf3fs {

struct Varint32 {
  uint32_t value;

  Varint32() = default;
  Varint32(uint32_t value)
      : value(value) {}

  operator uint32_t &() { return value; }
  operator uint32_t() const { return value; }
};

}  // namespace hf3fs

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::Varint32> : formatter<uint32_t> {
  template <typename FormatContext>
  auto format(hf3fs::Varint32 val, FormatContext &ctx) const {
    return formatter<uint32_t>::format(static_cast<uint32_t>(val), ctx);
  }
};

FMT_END_NAMESPACE
