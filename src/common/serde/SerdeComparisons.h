#pragma once

#include "Serde.h"

namespace hf3fs::serde {
template <SerdeType T>
bool equals(const T &a, const T &b) {
  auto handler = [&](auto field) {
    using Field = std::remove_cvref_t<decltype(field)>;
    const auto &l = a.*(Field::getter);
    const auto &r = b.*(Field::getter);
    if constexpr (SerdeType<std::decay_t<decltype(l)>> && !requires { l == r; }) {
      return serde::equals(l, r);
    } else {
      return l == r;
    }
  };
  return refl::Helper::iterate<T>(std::move(handler));
}

template <SerdeType T, typename Ordering = std::weak_ordering>
auto compare(const T &a, const T &b) {
  auto cmp = Ordering::equivalent;
  auto handler = [&](auto field) {
    using Field = std::remove_cvref_t<decltype(field)>;
    auto res = a.*(Field::getter) <=> b.*(Field::getter);
    if (res == 0) {
      return true;
    } else {
      cmp = res < 0 ? Ordering::less : Ordering::greater;
      return false;
    }
  };
  refl::Helper::iterate<T>(std::move(handler));
  return cmp;
}
}  // namespace hf3fs::serde
