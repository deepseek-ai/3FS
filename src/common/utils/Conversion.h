#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

namespace hf3fs {
template <typename To, typename From>
inline constexpr bool isSafeConvertTo(From from) noexcept {
  static_assert(std::is_integral_v<From> && std::is_integral_v<To>);
  static_assert(sizeof(From) <= sizeof(uint64_t) && sizeof(To) <= sizeof(uint64_t));
  if constexpr (std::is_signed_v<From> == std::is_signed_v<To>) {
    return from <= std::numeric_limits<To>::max() && from >= std::numeric_limits<To>::min();
  } else if constexpr (std::is_signed_v<From>) {
    return from >= 0 && static_cast<uint64_t>(from) <= std::numeric_limits<To>::max();
  } else {
    return from <= static_cast<uint64_t>(std::numeric_limits<To>::max());
  }
}
}  // namespace hf3fs
