#pragma once

#include <folly/Hash.h>

namespace hf3fs {

using int128_t = __int128;
using uint128_t = unsigned __int128;

}  // namespace hf3fs

template <>
struct std::hash<hf3fs::uint128_t> {
  constexpr size_t operator()(hf3fs::uint128_t const &i) const noexcept {
    auto const hi = static_cast<uint64_t>(i >> 64);
    auto const lo = static_cast<uint64_t>(i);
    return folly::hash::hash_128_to_64(hi, lo);
  }
};
template <>
struct std::hash<hf3fs::int128_t> {
  constexpr size_t operator()(hf3fs::int128_t const &i) const noexcept {
    return hash<hf3fs::uint128_t>{}(static_cast<hf3fs::uint128_t>(i));
  }
};
