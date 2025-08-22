#pragma once

#include <variant>

#include "common/utils/Nameof.hpp"
#include "common/utils/TypeTraits.h"

namespace hf3fs::serde {

template <class T>
inline constexpr std::string_view type_name_v = [] {
  if constexpr (requires { std::string_view{T::kTypeNameForSerde}; }) {
    return std::string_view{T::kTypeNameForSerde};
  } else {
    return nameof::nameof_short_type<T>();
  }
}();

template <class T>
inline constexpr auto variant_type_names_v = std::array<std::string_view, 0>{};
template <class... Ts>
inline constexpr auto variant_type_names_v<std::variant<Ts...>> = std::array{type_name_v<Ts>...};

template <class T>
inline uint8_t variantTypeNameToIndex(std::string_view in) {
  uint8_t i = 0;
  for (auto &name : variant_type_names_v<T>) {
    if (name == in) {
      break;
    }
    ++i;
  }
  return i;
}

}  // namespace hf3fs::serde
