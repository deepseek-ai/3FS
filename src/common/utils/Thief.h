#pragma once

#include <type_traits>

namespace hf3fs::thief {
namespace detail {

template <typename Tag>
struct Bridge {
#if defined(__GNUC__) && !defined(__clang__)
// Silence unnecessary warning
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-template-friend"
#endif
  friend consteval auto ADL(Bridge<Tag>);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
};

template <typename Tag, typename Store>
struct StealType {
  friend consteval auto ADL(Bridge<Tag>) { return std::type_identity<Store>{}; }
};

}  // namespace detail

template <typename Tag, typename Store>
using steal = decltype(detail::StealType<Tag, Store>{});

template <typename Tag>
using retrieve = typename decltype(ADL(detail::Bridge<Tag>{}))::type;

}  // namespace hf3fs::thief
