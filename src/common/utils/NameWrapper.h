#pragma once

#include <algorithm>
#include <string_view>

namespace hf3fs {
template <size_t N>
struct NameWrapper {
  constexpr NameWrapper(const char (&str)[N]) { std::copy_n(str, N, string); }
  constexpr operator std::string_view() const { return {string, N - 1}; }
  char string[N];
};
}  // namespace hf3fs
