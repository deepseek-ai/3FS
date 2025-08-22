#pragma once

#include <functional>
#include <type_traits>

namespace hf3fs {
template <typename P, typename T>
concept UnaryPredicate = std::is_same_v<bool, std::invoke_result_t<P, const T &>>;

// Selector is an alias of `std::function<bool(const T &)>`.
// Declare it as a new class so we can override its logical operators.
template <typename T, UnaryPredicate<T> F>
class Selector {
 public:
  Selector(F &&f)
      : predicate_(std::move(f)) {}

  bool operator()(const T &t) const { return predicate_(t); }

 private:
  F predicate_;
};

template <typename T, typename U0, typename U1>
inline auto operator&&(Selector<T, U0> &&s0, Selector<T, U1> &&s1) {
  auto predicate = [s0 = std::move(s0), s1 = std::move(s1)](const T &t) { return s0(t) && s1(t); };
  return Selector<T, decltype(predicate)>(std::move(predicate));
}

template <typename T, typename U0, typename U1>
inline auto operator||(Selector<T, U0> &&s0, Selector<T, U1> &&s1) {
  auto predicate = [s0 = std::move(s0), s1 = std::move(s1)](const T &t) { return s0(t) || s1(t); };
  return Selector<T, decltype(predicate)>(std::move(predicate));
}

template <typename T, typename U>
inline auto operator!(Selector<T, U> &&s) {
  auto predicate = [s = std::move(s)](const T &t) { return !s(t); };
  return Selector<T, decltype(predicate)>(std::move(predicate));
}

template <typename T>
inline auto allAccept() {
  static constexpr auto s = [](const T &) { return true; };
  return Selector<T, decltype(s)>(s);
}

template <typename T>
inline auto allReject() {
  static constexpr auto s = [](const T &) { return false; };
  return Selector<T, decltype(s)>(s);
}

template <typename T, UnaryPredicate<T> F>
inline auto makeSelector(F &&f) {
  return Selector<T, F>(std::move(f));
}
}  // namespace hf3fs
