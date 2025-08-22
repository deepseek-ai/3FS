#pragma once

#include <folly/hash/Hash.h>

#include "RobinHood.h"

namespace hf3fs {
struct StringHash {
  using is_transparent = void;  // enable heterogeneous overloads

  [[nodiscard]] auto operator()(std::string_view str) const noexcept -> uint64_t {
    return robin_hood::hash<std::string_view>{}(str);
  }
};

template <typename T>
using RHStringHashFlatMap = robin_hood::unordered_flat_map<std::string, T, StringHash, std::equal_to<>>;

template <typename T>
using RHStringHashNodeMap = robin_hood::unordered_node_map<std::string, T, StringHash, std::equal_to<>>;

template <typename T>
using RHStringHashMap = robin_hood::unordered_map<std::string, T, StringHash, std::equal_to<>>;

using RHStringHashSet = robin_hood::unordered_set<std::string, StringHash, std::equal_to<>>;

struct RobinHoodHasher {
  template <typename T>
  size_t operator()(const T &x) const {
    if constexpr (requires { robin_hood::hash<T>{}; }) {
      return robin_hood::hash<T>{}(x);
    } else {
      return std::hash<T>{}(x);
    }
  }
};

template <typename... Args>
auto robinhoodCombineHash(Args &&...args) {
  return folly::hash::hash_combine_generic(RobinHoodHasher{}, std::forward<Args>(args)...);
}
}  // namespace hf3fs
