#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

#include "common/utils/TypeTraits.h"

namespace hf3fs::refl {

template <size_t N = 64>
struct Rank : Rank<N - 1> {};
template <>
struct Rank<0> {};

template <class Tuple, class T>
struct Append;
template <class... Ts, class T>
struct Append<std::tuple<Ts...>, T> {
  using type = std::tuple<Ts..., T>;
};
template <class Tuple, class T>
using Append_t = typename Append<Tuple, T>::type;

// REFL_ADD find this function by ADL.
[[maybe_unused]] static std::tuple<> CollectField(::hf3fs::refl::Rank<0>);

#define REFL_NOW decltype(CollectField(::hf3fs::refl::Rank<>{}))
#define REFL_ADD(info)                                                   \
  static ::hf3fs::refl::Append_t<REFL_NOW, decltype(info)> CollectField( \
      ::hf3fs::refl::Rank<std::tuple_size_v<REFL_NOW> + 1>)
#define REFL_ADD_SAFE(info, t) \
  static ::hf3fs::refl::Append_t<t, decltype(info)> CollectField(::hf3fs::refl::Rank<std::tuple_size_v<t> + 1>)

struct Helper {
  template <class T>
  static decltype(CollectField(refl::Rank<>{})) getFieldInfo();
  template <class T>
  requires requires {
    { T::CollectField(refl::Rank<>{}) } -> is_specialization<std::tuple>;
  }
  static decltype(T::CollectField(refl::Rank<>{})) getFieldInfo();

  template <class T>
  using FieldInfoList = decltype(getFieldInfo<T>());

  template <typename T>
  static constexpr auto Size = std::tuple_size_v<FieldInfoList<T>>;

  template <class T, size_t I>
  using FieldInfo = std::tuple_element_t<I, FieldInfoList<T>>;

  template <typename T, bool Backwards = false, size_t I = 0>
  static constexpr auto iterate(auto &&f, auto &&...typeChanged) requires(Size<T> > 0 && I < Size<T>) {
    constexpr auto idx = Backwards ? Size<T> - 1 - I : I;
    auto t = FieldInfo<T, idx>{};
    if constexpr (I > 0 && sizeof...(typeChanged) > 0) {
      constexpr auto pre = Backwards ? Size<T> - I : I - 1;
      if constexpr (!std::is_same_v<member_pointer_to_class_t<FieldInfo<T, idx>::getter>,
                                    member_pointer_to_class_t<FieldInfo<T, pre>::getter>>) {
        auto &&first = getFirstParameter(typeChanged...);
        if constexpr (requires { bool(first()); }) {
          auto result = first();
          if (UNLIKELY(!result)) {
            return result;
          }
        } else {
          first();
        }
      }
    }
    static_assert(requires { f(t); });
    if constexpr (I + 1 < Size<T>) {
      if constexpr (requires { bool(f(t)); }) {
        auto result = f(t);
        if (UNLIKELY(!result)) {
          return result;
        }
      } else {
        f(t);
      }
      return iterate<T, Backwards, I + 1>(f, typeChanged...);
    } else {
      return f(t);
    }
  }

  template <typename T>
  static auto iterate(auto &&) requires(Size<T> == 0) {
    return;
  }

  template <typename T, bool Backwards = false, size_t I = 0>
  static auto visit(auto &&f) requires(Size<T> > 0 && I < Size<T>) {
    constexpr auto idx = Backwards ? Size<T> - 1 - I : I;
    auto t = FieldInfo<T, idx>{};
    if constexpr (requires { f(t); }) {
      return f(t);
    } else {
      return visit<T, Backwards, I + 1>(std::forward<decltype(f)>(f));
    }
  }

  template <typename T>
  static auto visit(auto &&) requires(Size<T> == 0) {
    return;
  }

  template <typename T, typename F, typename Generator, std::size_t... I>
  static auto applyImpl(F &&f, Generator &&gen, std::index_sequence<I...>) {
    return f(gen(FieldInfo<T, I>{})...);
  }

  template <typename T, typename F, typename Generator>
  static auto apply(F &&f, Generator &&gen) {
    return applyImpl<T>(std::forward<F>(f), std::forward<Generator>(gen), std::make_index_sequence<Size<T>>{});
  }
};

}  // namespace hf3fs::refl
