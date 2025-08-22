#pragma once

#include <span>

#include "TypeTraits.h"

namespace hf3fs {
template <template <typename...> typename DstContainer, typename T, typename F>
auto transformTo(std::span<T> src, F &&transformer) {
  using R = std::invoke_result_t<F, const T &>;
  if constexpr (std::is_same_v<DstContainer<R>, std::vector<R>>) {
    std::vector<R> dst;
    dst.reserve(src.size());
    std::transform(src.begin(), src.end(), std::back_inserter(dst), std::forward<F>(transformer));
    return dst;
  } else {
    DstContainer<R> dst;
    std::transform(src.begin(), src.end(), std::back_inserter(dst), std::forward<F>(transformer));
  }
}
}  // namespace hf3fs
