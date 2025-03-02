// take from
// https://github.com/ClickHouse/ClickHouse/blob/766a3a0e111fa075879d7a8e15e749b42f7c748a/base/base/strong_typedef.h
#pragma once

#include <fmt/core.h>
#include <fmt/format.h>
#include <folly/dynamic.h>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include "common/utils/Result.h"

namespace hf3fs {
template <typename T, typename Tag>
struct StrongTypedef {
 private:
  using Self = StrongTypedef;
  T t;

 public:
  using UnderlyingType = T;
  static constexpr auto kTypeName = Tag::kTypeName;
  using is_serde_copyable = void;

  template <class Enable = typename std::is_copy_constructible<T>::type>
  constexpr explicit StrongTypedef(const T &t_)
      : t(t_) {}
  template <class Enable = typename std::is_move_constructible<T>::type>
  constexpr explicit StrongTypedef(T &&t_)
      : t(std::move(t_)) {}

  template <class Enable = typename std::is_default_constructible<T>::type>
  constexpr StrongTypedef()
      : t() {}

  template <typename U>
  requires(std::same_as<T, U>) static Self from(U &&u) { return Self(std::forward<U>(u)); }

  constexpr StrongTypedef(const Self &) = default;
  constexpr StrongTypedef(Self &&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;

  Self &operator=(const Self &) = default;
  Self &operator=(Self &&) noexcept(std::is_nothrow_move_assignable_v<T>) = default;

  constexpr operator const T &() const { return t; }
  constexpr operator T &() { return t; }
  operator folly::dynamic() const { return t; }

  auto operator<=>(const Self &) const = default;

  // Non-default `operator<=>` could not be used for generating `operator==` implicitly.
  //
  // Why we do not instantiate `operator==` and `operator<=>` between two arithmetic types:
  // 1. for a < 5 where a is a `StrongTypedef<size_t>`, the `t <=> u` is a comparison of different signed integers.
  // 2. without `operator<=>` the comparison above will invoke `operator const T &` and the raw `operator==` (not a
  // function).
  // 3. then the compiler could infer `5` as an unsigned int and avoid `-Wsign-compare`.
  template <typename U>
  requires(!std::is_arithmetic_v<T> && !std::is_arithmetic_v<U>) bool operator==(const U &u) const { return t == u; }
  template <typename U>
  requires(!std::is_arithmetic_v<T> && !std::is_arithmetic_v<U>) auto operator<=>(const U &u) const { return t <=> u; }

  Self &operator++() noexcept requires(std::is_integral_v<T>) {
    ++t;
    return *this;
  }

  Self operator++(int) noexcept requires(std::is_integral_v<T>) { return Self(t++); }

  Self &operator--() noexcept requires(std::is_integral_v<T>) {
    --t;
    return *this;
  }

  Self operator--(int) noexcept requires(std::is_integral_v<T>) { return Self(t--); }

  constexpr T &toUnderType() { return t; }
  constexpr const T &toUnderType() const { return t; }

  constexpr T serdeToReadable() const { return t; }

  static Result<Self> serdeFromReadable(T t) { return Self(std::move(t)); }
};

template <typename T>
concept StrongTyped = requires(const T &t) {
  typename T::UnderlyingType;
  t.toUnderType();
  T::kTypeName;
};

template <typename T>
struct IsStrongTyped : std::false_type {};

template <StrongTyped T>
struct IsStrongTyped<T> : std::true_type {};
}  // namespace hf3fs

template <hf3fs::StrongTyped T>
struct std::hash<T> {
  size_t operator()(const T &x) const { return std::hash<typename T::UnderlyingType>()(x.toUnderType()); }
};

FMT_BEGIN_NAMESPACE

template <hf3fs::StrongTyped T>
struct formatter<T> : formatter<typename T::UnderlyingType> {
  template <typename FormatContext>
  auto format(const T &strongTyped, FormatContext &ctx) const {
    fmt::format_to(ctx.out(), "{}(", T::kTypeName);
    formatter<typename T::UnderlyingType>::format(strongTyped.toUnderType(), ctx);
    return fmt::format_to(ctx.out(), ")");
  }
};

template <hf3fs::StrongTyped T>
struct formatter<std::optional<T>> : formatter<typename T::UnderlyingType> {
  template <typename FormatContext>
  auto format(const std::optional<T> &strongTyped, FormatContext &ctx) const {
    if (strongTyped.has_value()) {
      fmt::format_to(ctx.out(), "{}(", T::kTypeName);
      formatter<typename T::UnderlyingType>::format(strongTyped->toUnderType(), ctx);
      return fmt::format_to(ctx.out(), ")");
    } else {
      return fmt::format_to(ctx.out(), "{}(std::nullopt)", T::kTypeName);
    }
  }
};

FMT_END_NAMESPACE

#define STRONG_TYPEDEF(T, D)                     \
  struct D##Tag {                                \
    static constexpr const char *kTypeName = #D; \
  };                                             \
  using D = hf3fs::StrongTypedef<T, D##Tag>

STRONG_TYPEDEF(char, TypedChar);
static_assert(hf3fs::IsStrongTyped<TypedChar>::value);
