//  __  __             _        ______                          _____
// |  \/  |           (_)      |  ____|                        / ____|_     _
// | \  / | __ _  __ _ _  ___  | |__   _ __  _   _ _ __ ___   | |   _| |_ _| |_
// | |\/| |/ _` |/ _` | |/ __| |  __| | '_ \| | | | '_ ` _ \  | |  |_   _|_   _|
// | |  | | (_| | (_| | | (__  | |____| | | | |_| | | | | | | | |____|_|   |_|
// |_|  |_|\__,_|\__, |_|\___| |______|_| |_|\__,_|_| |_| |_|  \_____|
//                __/ | https://github.com/Neargye/magic_enum
//               |___/  version 0.8.2
//
// Licensed under the MIT License <http://opensource.org/licenses/MIT>.
// SPDX-License-Identifier: MIT
// Copyright (c) 2019 - 2022 Daniil Goncharov <neargye@gmail.com>.
//
// Permission is hereby  granted, free of charge, to any  person obtaining a copy
// of this software and associated  documentation files (the "Software"), to deal
// in the Software  without restriction, including without  limitation the rights
// to  use, copy,  modify, merge,  publish, distribute,  sublicense, and/or  sell
// copies  of  the Software,  and  to  permit persons  to  whom  the Software  is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE  IS PROVIDED "AS  IS", WITHOUT WARRANTY  OF ANY KIND,  EXPRESS OR
// IMPLIED,  INCLUDING BUT  NOT  LIMITED TO  THE  WARRANTIES OF  MERCHANTABILITY,
// FITNESS FOR  A PARTICULAR PURPOSE AND  NONINFRINGEMENT. IN NO EVENT  SHALL THE
// AUTHORS  OR COPYRIGHT  HOLDERS  BE  LIABLE FOR  ANY  CLAIM,  DAMAGES OR  OTHER
// LIABILITY, WHETHER IN AN ACTION OF  CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE  OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef NEARGYE_MAGIC_ENUM_HPP
#define NEARGYE_MAGIC_ENUM_HPP

#define MAGIC_ENUM_VERSION_MAJOR 0
#define MAGIC_ENUM_VERSION_MINOR 8
#define MAGIC_ENUM_VERSION_PATCH 2

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#if defined(MAGIC_ENUM_CONFIG_FILE)
#include MAGIC_ENUM_CONFIG_FILE
#endif

#if !defined(MAGIC_ENUM_USING_ALIAS_OPTIONAL)
#include <optional>
#endif
#if !defined(MAGIC_ENUM_USING_ALIAS_STRING)
#include <string>
#endif
#if !defined(MAGIC_ENUM_USING_ALIAS_STRING_VIEW)
#include <string_view>
#endif
#if !defined(MAGIC_ENUM_NO_STREAMS)
#include <iosfwd>
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wenum-constexpr-conversion"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"  // May be used uninitialized 'return {};'.
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 26495)  // Variable 'static_string<N>::chars_' is uninitialized.
#pragma warning(disable : 28020)  // Arithmetic overflow: Using operator '-' on a 4 byte value and then casting the
                                  // result to a 8 byte value.
#pragma warning(disable : 26451)  // The expression '0<=_Param_(1)&&_Param_(1)<=1-1' is not true at this call.
#pragma warning(disable : 4514)   // Unreferenced inline function has been removed.
#endif

// Checks magic_enum compiler compatibility.
#if defined(__clang__) && __clang_major__ >= 5 || defined(__GNUC__) && __GNUC__ >= 9 || \
    defined(_MSC_VER) && _MSC_VER >= 1910
#undef MAGIC_ENUM_SUPPORTED
#define MAGIC_ENUM_SUPPORTED 1
#endif

// Checks magic_enum compiler aliases compatibility.
#if defined(__clang__) && __clang_major__ >= 5 || defined(__GNUC__) && __GNUC__ >= 9 || \
    defined(_MSC_VER) && _MSC_VER >= 1920
#undef MAGIC_ENUM_SUPPORTED_ALIASES
#define MAGIC_ENUM_SUPPORTED_ALIASES 1
#endif

// Enum value must be greater or equals than MAGIC_ENUM_RANGE_MIN. By default MAGIC_ENUM_RANGE_MIN = -128.
// If need another min range for all enum types by default, redefine the macro MAGIC_ENUM_RANGE_MIN.
#if !defined(MAGIC_ENUM_RANGE_MIN)
#define MAGIC_ENUM_RANGE_MIN -128
#endif

// Enum value must be less or equals than MAGIC_ENUM_RANGE_MAX. By default MAGIC_ENUM_RANGE_MAX = 128.
// If need another max range for all enum types by default, redefine the macro MAGIC_ENUM_RANGE_MAX.
#if !defined(MAGIC_ENUM_RANGE_MAX)
#define MAGIC_ENUM_RANGE_MAX 128
#endif

namespace magic_enum {

// If need another optional type, define the macro MAGIC_ENUM_USING_ALIAS_OPTIONAL.
#if defined(MAGIC_ENUM_USING_ALIAS_OPTIONAL)
MAGIC_ENUM_USING_ALIAS_OPTIONAL
#else
using std::optional;
#endif

// If need another string_view type, define the macro MAGIC_ENUM_USING_ALIAS_STRING_VIEW.
#if defined(MAGIC_ENUM_USING_ALIAS_STRING_VIEW)
MAGIC_ENUM_USING_ALIAS_STRING_VIEW
#else
using std::string_view;
#endif

// If need another string type, define the macro MAGIC_ENUM_USING_ALIAS_STRING.
#if defined(MAGIC_ENUM_USING_ALIAS_STRING)
MAGIC_ENUM_USING_ALIAS_STRING
#else
using std::string;
#endif

namespace customize {

// Enum value must be in range [MAGIC_ENUM_RANGE_MIN, MAGIC_ENUM_RANGE_MAX]. By default MAGIC_ENUM_RANGE_MIN = -128,
// MAGIC_ENUM_RANGE_MAX = 128. If need another range for all enum types by default, redefine the macro
// MAGIC_ENUM_RANGE_MIN and MAGIC_ENUM_RANGE_MAX. If need another range for specific enum type, add specialization
// enum_range for necessary enum type.
template <typename E>
struct enum_range {
  static_assert(std::is_enum_v<E>, "magic_enum::customize::enum_range requires enum type.");
  static constexpr int min = MAGIC_ENUM_RANGE_MIN;
  static constexpr int max = MAGIC_ENUM_RANGE_MAX;
  static_assert(max > min, "magic_enum::customize::enum_range requires max > min.");
};

static_assert(MAGIC_ENUM_RANGE_MAX > MAGIC_ENUM_RANGE_MIN,
              "MAGIC_ENUM_RANGE_MAX must be greater than MAGIC_ENUM_RANGE_MIN.");
static_assert((MAGIC_ENUM_RANGE_MAX - MAGIC_ENUM_RANGE_MIN) < (std::numeric_limits<std::uint16_t>::max)(),
              "MAGIC_ENUM_RANGE must be less than UINT16_MAX.");

namespace detail {

enum class customize_tag { default_tag, invalid_tag, custom_tag };

}  // namespace detail

class customize_t : public std::pair<detail::customize_tag, string_view> {
 public:
  constexpr customize_t(string_view srt)
      : std::pair<detail::customize_tag, string_view>{detail::customize_tag::custom_tag, srt} {}
  constexpr customize_t(const char *srt)
      : customize_t{string_view{srt}} {}
  constexpr customize_t(detail::customize_tag tag)
      : std::pair<detail::customize_tag, string_view>{tag, string_view{}} {
    assert(tag != detail::customize_tag::custom_tag);
  }
};

// Default customize.
inline constexpr auto default_tag = customize_t{detail::customize_tag::default_tag};
// Invalid customize.
inline constexpr auto invalid_tag = customize_t{detail::customize_tag::invalid_tag};

// If need custom names for enum, add specialization enum_name for necessary enum type.
template <typename E>
constexpr customize_t enum_name(E) noexcept {
  return default_tag;
}

// If need custom type name for enum, add specialization enum_type_name for necessary enum type.
template <typename E>
constexpr customize_t enum_type_name() noexcept {
  return default_tag;
}

}  // namespace customize

namespace detail {

template <auto V, typename E = std::decay_t<decltype(V)>, std::enable_if_t<std::is_enum_v<E>, int> = 0>
using enum_constant = std::integral_constant<E, V>;

enum class value_type { default_value, flags_value };

template <typename... T>
inline constexpr bool always_false_v = false;

template <typename T>
struct supported
#if defined(MAGIC_ENUM_SUPPORTED) && MAGIC_ENUM_SUPPORTED || defined(MAGIC_ENUM_NO_CHECK_SUPPORT)
    : std::true_type {
};
#else
    : std::false_type {
};
#endif

template <typename T, typename = void>
struct has_is_flags : std::false_type {};

template <typename T>
struct has_is_flags<T, std::void_t<decltype(customize::enum_range<T>::is_flags)>>
    : std::bool_constant<std::is_same_v<bool, std::decay_t<decltype(customize::enum_range<T>::is_flags)>>> {};

template <typename T, typename = void>
struct range_min : std::integral_constant<int, MAGIC_ENUM_RANGE_MIN> {};

template <typename T>
struct range_min<T, std::void_t<decltype(customize::enum_range<T>::min)>>
    : std::integral_constant<decltype(customize::enum_range<T>::min), customize::enum_range<T>::min> {};

template <typename T, typename = void>
struct range_max : std::integral_constant<int, MAGIC_ENUM_RANGE_MAX> {};

template <typename T>
struct range_max<T, std::void_t<decltype(customize::enum_range<T>::max)>>
    : std::integral_constant<decltype(customize::enum_range<T>::max), customize::enum_range<T>::max> {};

template <std::uint16_t N>
class static_string {
 public:
  constexpr explicit static_string(string_view str) noexcept
      : static_string{str, std::make_integer_sequence<std::uint16_t, N>{}} {
    assert(str.size() == N);
  }

  constexpr const char *data() const noexcept { return chars_; }

  constexpr std::uint16_t size() const noexcept { return N; }

  constexpr operator string_view() const noexcept { return {data(), size()}; }

 private:
  template <std::uint16_t... I>
  constexpr static_string(string_view str, std::integer_sequence<std::uint16_t, I...>) noexcept
      : chars_{str[I]..., '\0'} {}

  char chars_[static_cast<std::size_t>(N) + 1];
};

template <>
class static_string<0> {
 public:
  constexpr explicit static_string() = default;

  constexpr explicit static_string(string_view) noexcept {}

  constexpr const char *data() const noexcept { return nullptr; }

  constexpr std::uint16_t size() const noexcept { return 0; }

  constexpr operator string_view() const noexcept { return {}; }
};

constexpr string_view pretty_name(string_view name) noexcept {
  for (std::size_t i = name.size(); i > 0; --i) {
    if (!((name[i - 1] >= '0' && name[i - 1] <= '9') || (name[i - 1] >= 'a' && name[i - 1] <= 'z') ||
          (name[i - 1] >= 'A' && name[i - 1] <= 'Z') ||
#if defined(MAGIC_ENUM_ENABLE_NONASCII)
          (name[i - 1] & 0x80) ||
#endif
          (name[i - 1] == '_'))) {
      name.remove_prefix(i);
      break;
    }
  }

  if (name.size() > 0 && ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z') ||
#if defined(MAGIC_ENUM_ENABLE_NONASCII)
                          (name[0]) & 0x80) ||
#endif
      (name[0] == '_'))) {
    return name;
  }
  return {};  // Invalid name.
}

class case_insensitive {
  static constexpr char to_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
  }

 public:
  template <typename L, typename R>
  constexpr auto operator()([[maybe_unused]] L lhs, [[maybe_unused]] R rhs) const noexcept
      -> std::enable_if_t<std::is_same_v<std::decay_t<L>, char> && std::is_same_v<std::decay_t<R>, char>, bool> {
#if defined(MAGIC_ENUM_ENABLE_NONASCII)
    static_assert(always_false_v<L, R>, "magic_enum::case_insensitive not supported Non-ASCII feature.");
    return false;
#else
    return to_lower(lhs) == to_lower(rhs);
#endif
  }
};

constexpr std::size_t find(string_view str, char c) noexcept {
#if defined(__clang__) && __clang_major__ < 9 && defined(__GLIBCXX__) || \
    defined(_MSC_VER) && _MSC_VER < 1920 && !defined(__clang__)
  // https://stackoverflow.com/questions/56484834/constexpr-stdstring-viewfind-last-of-doesnt-work-on-clang-8-with-libstdc
  // https://developercommunity.visualstudio.com/content/problem/360432/vs20178-regression-c-failed-in-test.html
  constexpr bool workaround = true;
#else
  constexpr bool workaround = false;
#endif

  if constexpr (workaround) {
    for (std::size_t i = 0; i < str.size(); ++i) {
      if (str[i] == c) {
        return i;
      }
    }

    return string_view::npos;
  } else {
    return str.find(c);
  }
}

template <typename T, std::size_t N, std::size_t... I>
constexpr std::array<std::remove_cv_t<T>, N> to_array(T (&a)[N], std::index_sequence<I...>) noexcept {
  return {{a[I]...}};
}

template <typename BinaryPredicate>
constexpr bool is_default_predicate() noexcept {
  return std::is_same_v<std::decay_t<BinaryPredicate>, std::equal_to<string_view::value_type>> ||
         std::is_same_v<std::decay_t<BinaryPredicate>, std::equal_to<>>;
}

template <typename BinaryPredicate>
constexpr bool is_nothrow_invocable() {
  return is_default_predicate<BinaryPredicate>() || std::is_nothrow_invocable_r_v<bool, BinaryPredicate, char, char>;
}

template <typename BinaryPredicate>
constexpr bool cmp_equal(string_view lhs,
                         string_view rhs,
                         [[maybe_unused]] BinaryPredicate &&p) noexcept(is_nothrow_invocable<BinaryPredicate>()) {
#if defined(_MSC_VER) && _MSC_VER < 1920 && !defined(__clang__)
  // https://developercommunity.visualstudio.com/content/problem/360432/vs20178-regression-c-failed-in-test.html
  // https://developercommunity.visualstudio.com/content/problem/232218/c-constexpr-string-view.html
  constexpr bool workaround = true;
#else
  constexpr bool workaround = false;
#endif

  if constexpr (!is_default_predicate<BinaryPredicate>() || workaround) {
    if (lhs.size() != rhs.size()) {
      return false;
    }

    const auto size = lhs.size();
    for (std::size_t i = 0; i < size; ++i) {
      if (!p(lhs[i], rhs[i])) {
        return false;
      }
    }

    return true;
  } else {
    return lhs == rhs;
  }
}

template <typename L, typename R>
constexpr bool cmp_less(L lhs, R rhs) noexcept {
  static_assert(std::is_integral_v<L> && std::is_integral_v<R>, "magic_enum::detail::cmp_less requires integral type.");

  if constexpr (std::is_signed_v<L> == std::is_signed_v<R>) {
    // If same signedness (both signed or both unsigned).
    return lhs < rhs;
  } else if constexpr (std::is_same_v<L, bool>) {  // bool special case
    return static_cast<R>(lhs) < rhs;
  } else if constexpr (std::is_same_v<R, bool>) {  // bool special case
    return lhs < static_cast<L>(rhs);
  } else if constexpr (std::is_signed_v<R>) {
    // If 'right' is negative, then result is 'false', otherwise cast & compare.
    return rhs > 0 && lhs < static_cast<std::make_unsigned_t<R>>(rhs);
  } else {
    // If 'left' is negative, then result is 'true', otherwise cast & compare.
    return lhs < 0 || static_cast<std::make_unsigned_t<L>>(lhs) < rhs;
  }
}

template <typename I>
constexpr I log2(I value) noexcept {
  static_assert(std::is_integral_v<I>, "magic_enum::detail::log2 requires integral type.");

  if constexpr (std::is_same_v<I, bool>) {  // bool special case
    return assert(false), value;
  } else {
    auto ret = I{0};
    for (; value > I{1}; value >>= I{1}, ++ret) {
    }

    return ret;
  }
}

template <typename T>
inline constexpr bool is_enum_v = std::is_enum_v<T> &&std::is_same_v<T, std::decay_t<T>>;

template <typename E>
constexpr auto n() noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::n requires enum type.");

  if constexpr (supported<E>::value) {
#if defined(__clang__) || defined(__GNUC__)
    constexpr auto name = pretty_name({__PRETTY_FUNCTION__, sizeof(__PRETTY_FUNCTION__) - 2});
#elif defined(_MSC_VER)
    constexpr auto name = pretty_name({__FUNCSIG__, sizeof(__FUNCSIG__) - 17});
#else
    constexpr auto name = string_view{};
#endif
    return name;
  } else {
    return string_view{};  // Unsupported compiler or Invalid customize.
  }
}

template <typename E>
constexpr auto type_name() noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::type_name requires enum type.");

  [[maybe_unused]] constexpr auto custom = customize::enum_type_name<E>();
  static_assert(std::is_same_v<std::decay_t<decltype(custom)>, customize::customize_t>,
                "magic_enum::customize requires customize_t type.");
  if constexpr (custom.first == customize::detail::customize_tag::custom_tag) {
    constexpr auto name = custom.second;
    static_assert(!name.empty(), "magic_enum::customize requires not empty string.");
    return static_string<name.size()>{name};
  } else if constexpr (custom.first == customize::detail::customize_tag::invalid_tag) {
    return static_string<0>{};
  } else if constexpr (custom.first == customize::detail::customize_tag::default_tag) {
    constexpr auto name = n<E>();
    return static_string<name.size()>{name};
  } else {
    static_assert(detail::always_false_v<E>, "magic_enum::customize invalid.");
  }
}

template <typename E>
inline constexpr auto type_name_v = type_name<E>();

template <typename E, E V>
constexpr auto n() noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::n requires enum type.");

  if constexpr (supported<E>::value) {
#if defined(__clang__) || defined(__GNUC__)
    constexpr auto name = pretty_name({__PRETTY_FUNCTION__, sizeof(__PRETTY_FUNCTION__) - 2});
#elif defined(_MSC_VER)
    constexpr auto name = pretty_name({__FUNCSIG__, sizeof(__FUNCSIG__) - 17});
#else
    constexpr auto name = string_view{};
#endif
    return name;
  } else {
    return string_view{};  // Unsupported compiler or Invalid customize.
  }
}

template <typename E, E V>
constexpr auto enum_name() noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::enum_name requires enum type.");

  [[maybe_unused]] constexpr auto custom = customize::enum_name<E>(V);
  static_assert(std::is_same_v<std::decay_t<decltype(custom)>, customize::customize_t>,
                "magic_enum::customize requires customize_t type.");
  if constexpr (custom.first == customize::detail::customize_tag::custom_tag) {
    constexpr auto name = custom.second;
    static_assert(!name.empty(), "magic_enum::customize requires not empty string.");
    return static_string<name.size()>{name};
  } else if constexpr (custom.first == customize::detail::customize_tag::invalid_tag) {
    return static_string<0>{};
  } else if constexpr (custom.first == customize::detail::customize_tag::default_tag) {
    constexpr auto name = n<E, V>();
    return static_string<name.size()>{name};
  } else {
    static_assert(detail::always_false_v<E>, "magic_enum::customize invalid.");
  }
}

template <typename E, E V>
inline constexpr auto enum_name_v = enum_name<E, V>();

template <typename E, auto V>
constexpr bool is_valid() noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::is_valid requires enum type.");

#if defined(__clang__) && __clang_major__ >= 16
  // https://reviews.llvm.org/D130058, https://reviews.llvm.org/D131307
  constexpr E v = __builtin_bit_cast(E, V);
#else
  constexpr E v = static_cast<E>(V);
#endif
  [[maybe_unused]] constexpr auto custom = customize::enum_name<E>(v);
  static_assert(std::is_same_v<std::decay_t<decltype(custom)>, customize::customize_t>,
                "magic_enum::customize requires customize_t type.");
  if constexpr (custom.first == customize::detail::customize_tag::custom_tag) {
    constexpr auto name = custom.second;
    static_assert(!name.empty(), "magic_enum::customize requires not empty string.");
    return name.size() != 0;
  } else if constexpr (custom.first == customize::detail::customize_tag::default_tag) {
    return n<E, v>().size() != 0;
  } else {
    return false;
  }
}

template <typename E, int O, bool IsFlags, typename U = std::underlying_type_t<E>>
constexpr U ualue(std::size_t i) noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::ualue requires enum type.");

  if constexpr (std::is_same_v<U, bool>) {  // bool special case
    static_assert(O == 0, "magic_enum::detail::ualue requires valid offset.");

    return static_cast<U>(i);
  } else if constexpr (IsFlags) {
    return static_cast<U>(U{1} << static_cast<U>(static_cast<int>(i) + O));
  } else {
    return static_cast<U>(static_cast<int>(i) + O);
  }
}

template <typename E, int O, bool IsFlags, typename U = std::underlying_type_t<E>>
constexpr E value(std::size_t i) noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::value requires enum type.");

  return static_cast<E>(ualue<E, O, IsFlags>(i));
}

template <typename E, bool IsFlags, typename U = std::underlying_type_t<E>>
constexpr int reflected_min() noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::reflected_min requires enum type.");

  if constexpr (IsFlags) {
    return 0;
  } else {
    constexpr auto lhs = range_min<E>::value;
    constexpr auto rhs = (std::numeric_limits<U>::min)();

    if constexpr (cmp_less(rhs, lhs)) {
      return lhs;
    } else {
      return rhs;
    }
  }
}

template <typename E, bool IsFlags, typename U = std::underlying_type_t<E>>
constexpr int reflected_max() noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::reflected_max requires enum type.");

  if constexpr (IsFlags) {
    return std::numeric_limits<U>::digits - 1;
  } else {
    constexpr auto lhs = range_max<E>::value;
    constexpr auto rhs = (std::numeric_limits<U>::max)();

    if constexpr (cmp_less(lhs, rhs)) {
      return lhs;
    } else {
      return rhs;
    }
  }
}

template <typename E, bool IsFlags>
inline constexpr auto reflected_min_v = reflected_min<E, IsFlags>();

template <typename E, bool IsFlags>
inline constexpr auto reflected_max_v = reflected_max<E, IsFlags>();

template <std::size_t N>
constexpr std::size_t values_count(const bool (&valid)[N]) noexcept {
  auto count = std::size_t{0};
  for (std::size_t i = 0; i < N; ++i) {
    if (valid[i]) {
      ++count;
    }
  }

  return count;
}

template <typename E, bool IsFlags, int Min, std::size_t... I>
constexpr auto values(std::index_sequence<I...>) noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::values requires enum type.");
  constexpr bool valid[sizeof...(I)] = {is_valid<E, ualue<E, Min, IsFlags>(I)>()...};
  constexpr std::size_t count = values_count(valid);

  if constexpr (count > 0) {
    E values[count] = {};
    for (std::size_t i = 0, v = 0; v < count; ++i) {
      if (valid[i]) {
        values[v++] = value<E, Min, IsFlags>(i);
      }
    }

    return to_array(values, std::make_index_sequence<count>{});
  } else {
    return std::array<E, 0>{};
  }
}

template <typename E, bool IsFlags, typename U = std::underlying_type_t<E>>
constexpr auto values() noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::values requires enum type.");
  constexpr auto min = reflected_min_v<E, IsFlags>;
  constexpr auto max = reflected_max_v<E, IsFlags>;
  constexpr auto range_size = max - min + 1;
  static_assert(range_size > 0, "magic_enum::enum_range requires valid size.");
  static_assert(range_size < (std::numeric_limits<std::uint16_t>::max)(),
                "magic_enum::enum_range requires valid size.");

  return values<E, IsFlags, min>(std::make_index_sequence<range_size>{});
}

template <typename E, typename U = std::underlying_type_t<E>>
constexpr bool is_flags_enum() noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::is_flags_enum requires enum type.");

  if constexpr (has_is_flags<E>::value) {
    return customize::enum_range<E>::is_flags;
  } else if constexpr (std::is_same_v<U, bool>) {  // bool special case
    return false;
  } else {
#if defined(MAGIC_ENUM_NO_CHECK_FLAGS)
    return false;
#else
    constexpr auto flags_values = values<E, true>();
    constexpr auto default_values = values<E, false>();
    if (flags_values.size() == 0 || default_values.size() > flags_values.size()) {
      return false;
    }
    for (std::size_t i = 0; i < default_values.size(); ++i) {
      const auto v = static_cast<U>(default_values[i]);
      if (v != 0 && (v & (v - 1)) != 0) {
        return false;
      }
    }
    return flags_values.size() > 0;
#endif
  }
}

template <typename E>
inline constexpr bool is_flags_v = is_flags_enum<E>();

template <typename E>
inline constexpr std::array values_v = values<E, is_flags_v<E>>();

template <typename E, typename D = std::decay_t<E>>
using values_t = decltype((values_v<D>));

template <typename E>
inline constexpr auto count_v = values_v<E>.size();

template <typename E, typename U = std::underlying_type_t<E>>
inline constexpr auto min_v = (count_v<E> > 0) ? static_cast<U>(values_v<E>.front()) : U{0};

template <typename E, typename U = std::underlying_type_t<E>>
inline constexpr auto max_v = (count_v<E> > 0) ? static_cast<U>(values_v<E>.back()) : U{0};

template <typename E, std::size_t... I>
constexpr auto names(std::index_sequence<I...>) noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::names requires enum type.");

  return std::array<string_view, sizeof...(I)>{{enum_name_v<E, values_v<E>[I]>...}};
}

template <typename E>
inline constexpr std::array names_v = names<E>(std::make_index_sequence<count_v<E>>{});

template <typename E, typename D = std::decay_t<E>>
using names_t = decltype((names_v<D>));

template <typename E, std::size_t... I>
constexpr auto entries(std::index_sequence<I...>) noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::entries requires enum type.");

  return std::array<std::pair<E, string_view>, sizeof...(I)>{{{values_v<E>[I], enum_name_v<E, values_v<E>[I]>}...}};
}

template <typename E>
inline constexpr std::array entries_v = entries<E>(std::make_index_sequence<count_v<E>>{});

template <typename E, typename D = std::decay_t<E>>
using entries_t = decltype((entries_v<D>));

template <typename E, typename U = std::underlying_type_t<E>>
constexpr bool is_sparse() noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::is_sparse requires enum type.");

  if constexpr (count_v<E> == 0) {
    return false;
  } else if constexpr (std::is_same_v<U, bool>) {  // bool special case
    return false;
  } else {
    constexpr auto max = is_flags_v<E> ? log2(max_v<E>) : max_v<E>;
    constexpr auto min = is_flags_v<E> ? log2(min_v<E>) : min_v<E>;
    constexpr auto range_size = max - min + 1;

    return range_size != count_v<E>;
  }
}

template <typename E>
inline constexpr bool is_sparse_v = is_sparse<E>();

template <typename E, typename U = std::underlying_type_t<E>>
constexpr U values_ors() noexcept {
  static_assert(is_enum_v<E>, "magic_enum::detail::values_ors requires enum type.");

  auto ors = U{0};
  for (std::size_t i = 0; i < count_v<E>; ++i) {
    ors |= static_cast<U>(values_v<E>[i]);
  }

  return ors;
}

template <bool, typename R>
struct enable_if_enum {};

template <typename R>
struct enable_if_enum<true, R> {
  using type = R;
  static_assert(supported<R>::value,
                "magic_enum unsupported compiler (https://github.com/Neargye/magic_enum#compiler-compatibility).");
};

template <typename T, typename R, typename BinaryPredicate = std::equal_to<>, typename D = std::decay_t<T>>
using enable_if_t =
    typename enable_if_enum<std::is_enum_v<D> && std::is_invocable_r_v<bool, BinaryPredicate, char, char>, R>::type;

template <typename T,
          typename R,
          value_type VT,
          typename BinaryPredicate = std::equal_to<>,
          typename D = std::decay_t<T>>
using enable_if_default_t = typename enable_if_enum<std::is_enum_v<D> && VT == value_type::default_value &&
                                                        std::is_invocable_r_v<bool, BinaryPredicate, char, char>,
                                                    R>::type;

template <typename T,
          typename R,
          value_type VT,
          typename BinaryPredicate = std::equal_to<>,
          typename D = std::decay_t<T>>
using enable_if_flags_t = typename enable_if_enum<std::is_enum_v<D> && VT == value_type::flags_value &&
                                                      std::is_invocable_r_v<bool, BinaryPredicate, char, char>,
                                                  R>::type;

template <typename T, std::enable_if_t<std::is_enum_v<std::decay_t<T>>, int> = 0>
using enum_concept = T;

template <typename T, bool = std::is_enum_v<T>>
struct is_scoped_enum : std::false_type {};

template <typename T>
struct is_scoped_enum<T, true> : std::bool_constant<!std::is_convertible_v<T, std::underlying_type_t<T>>> {};

template <typename T, bool = std::is_enum_v<T>>
struct is_unscoped_enum : std::false_type {};

template <typename T>
struct is_unscoped_enum<T, true> : std::bool_constant<std::is_convertible_v<T, std::underlying_type_t<T>>> {};

template <typename T, bool = std::is_enum_v<std::decay_t<T>>>
struct underlying_type {};

template <typename T>
struct underlying_type<T, true> : std::underlying_type<std::decay_t<T>> {};

#if defined(MAGIC_ENUM_ENABLE_HASH)

template <typename T>
inline constexpr bool has_hash = true;

template <typename Value, typename = void>
struct constexpr_hash_t;

template <typename Value>
struct constexpr_hash_t<Value, std::enable_if_t<is_enum_v<Value>>> {
  constexpr auto operator()(Value value) const noexcept {
    using U = typename underlying_type<Value>::type;
    if constexpr (std::is_same_v<U, bool>) {  // bool special case
      return static_cast<std::size_t>(value);
    } else {
      return static_cast<U>(value);
    }
  }
  using secondary_hash = constexpr_hash_t;
};

template <typename Value>
struct constexpr_hash_t<Value, std::enable_if_t<std::is_same_v<Value, string_view>>> {
  static constexpr std::uint32_t crc_table[256]{
      0x00000000L, 0x77073096L, 0xee0e612cL, 0x990951baL, 0x076dc419L, 0x706af48fL, 0xe963a535L, 0x9e6495a3L,
      0x0edb8832L, 0x79dcb8a4L, 0xe0d5e91eL, 0x97d2d988L, 0x09b64c2bL, 0x7eb17cbdL, 0xe7b82d07L, 0x90bf1d91L,
      0x1db71064L, 0x6ab020f2L, 0xf3b97148L, 0x84be41deL, 0x1adad47dL, 0x6ddde4ebL, 0xf4d4b551L, 0x83d385c7L,
      0x136c9856L, 0x646ba8c0L, 0xfd62f97aL, 0x8a65c9ecL, 0x14015c4fL, 0x63066cd9L, 0xfa0f3d63L, 0x8d080df5L,
      0x3b6e20c8L, 0x4c69105eL, 0xd56041e4L, 0xa2677172L, 0x3c03e4d1L, 0x4b04d447L, 0xd20d85fdL, 0xa50ab56bL,
      0x35b5a8faL, 0x42b2986cL, 0xdbbbc9d6L, 0xacbcf940L, 0x32d86ce3L, 0x45df5c75L, 0xdcd60dcfL, 0xabd13d59L,
      0x26d930acL, 0x51de003aL, 0xc8d75180L, 0xbfd06116L, 0x21b4f4b5L, 0x56b3c423L, 0xcfba9599L, 0xb8bda50fL,
      0x2802b89eL, 0x5f058808L, 0xc60cd9b2L, 0xb10be924L, 0x2f6f7c87L, 0x58684c11L, 0xc1611dabL, 0xb6662d3dL,
      0x76dc4190L, 0x01db7106L, 0x98d220bcL, 0xefd5102aL, 0x71b18589L, 0x06b6b51fL, 0x9fbfe4a5L, 0xe8b8d433L,
      0x7807c9a2L, 0x0f00f934L, 0x9609a88eL, 0xe10e9818L, 0x7f6a0dbbL, 0x086d3d2dL, 0x91646c97L, 0xe6635c01L,
      0x6b6b51f4L, 0x1c6c6162L, 0x856530d8L, 0xf262004eL, 0x6c0695edL, 0x1b01a57bL, 0x8208f4c1L, 0xf50fc457L,
      0x65b0d9c6L, 0x12b7e950L, 0x8bbeb8eaL, 0xfcb9887cL, 0x62dd1ddfL, 0x15da2d49L, 0x8cd37cf3L, 0xfbd44c65L,
      0x4db26158L, 0x3ab551ceL, 0xa3bc0074L, 0xd4bb30e2L, 0x4adfa541L, 0x3dd895d7L, 0xa4d1c46dL, 0xd3d6f4fbL,
      0x4369e96aL, 0x346ed9fcL, 0xad678846L, 0xda60b8d0L, 0x44042d73L, 0x33031de5L, 0xaa0a4c5fL, 0xdd0d7cc9L,
      0x5005713cL, 0x270241aaL, 0xbe0b1010L, 0xc90c2086L, 0x5768b525L, 0x206f85b3L, 0xb966d409L, 0xce61e49fL,
      0x5edef90eL, 0x29d9c998L, 0xb0d09822L, 0xc7d7a8b4L, 0x59b33d17L, 0x2eb40d81L, 0xb7bd5c3bL, 0xc0ba6cadL,
      0xedb88320L, 0x9abfb3b6L, 0x03b6e20cL, 0x74b1d29aL, 0xead54739L, 0x9dd277afL, 0x04db2615L, 0x73dc1683L,
      0xe3630b12L, 0x94643b84L, 0x0d6d6a3eL, 0x7a6a5aa8L, 0xe40ecf0bL, 0x9309ff9dL, 0x0a00ae27L, 0x7d079eb1L,
      0xf00f9344L, 0x8708a3d2L, 0x1e01f268L, 0x6906c2feL, 0xf762575dL, 0x806567cbL, 0x196c3671L, 0x6e6b06e7L,
      0xfed41b76L, 0x89d32be0L, 0x10da7a5aL, 0x67dd4accL, 0xf9b9df6fL, 0x8ebeeff9L, 0x17b7be43L, 0x60b08ed5L,
      0xd6d6a3e8L, 0xa1d1937eL, 0x38d8c2c4L, 0x4fdff252L, 0xd1bb67f1L, 0xa6bc5767L, 0x3fb506ddL, 0x48b2364bL,
      0xd80d2bdaL, 0xaf0a1b4cL, 0x36034af6L, 0x41047a60L, 0xdf60efc3L, 0xa867df55L, 0x316e8eefL, 0x4669be79L,
      0xcb61b38cL, 0xbc66831aL, 0x256fd2a0L, 0x5268e236L, 0xcc0c7795L, 0xbb0b4703L, 0x220216b9L, 0x5505262fL,
      0xc5ba3bbeL, 0xb2bd0b28L, 0x2bb45a92L, 0x5cb36a04L, 0xc2d7ffa7L, 0xb5d0cf31L, 0x2cd99e8bL, 0x5bdeae1dL,
      0x9b64c2b0L, 0xec63f226L, 0x756aa39cL, 0x026d930aL, 0x9c0906a9L, 0xeb0e363fL, 0x72076785L, 0x05005713L,
      0x95bf4a82L, 0xe2b87a14L, 0x7bb12baeL, 0x0cb61b38L, 0x92d28e9bL, 0xe5d5be0dL, 0x7cdcefb7L, 0x0bdbdf21L,
      0x86d3d2d4L, 0xf1d4e242L, 0x68ddb3f8L, 0x1fda836eL, 0x81be16cdL, 0xf6b9265bL, 0x6fb077e1L, 0x18b74777L,
      0x88085ae6L, 0xff0f6a70L, 0x66063bcaL, 0x11010b5cL, 0x8f659effL, 0xf862ae69L, 0x616bffd3L, 0x166ccf45L,
      0xa00ae278L, 0xd70dd2eeL, 0x4e048354L, 0x3903b3c2L, 0xa7672661L, 0xd06016f7L, 0x4969474dL, 0x3e6e77dbL,
      0xaed16a4aL, 0xd9d65adcL, 0x40df0b66L, 0x37d83bf0L, 0xa9bcae53L, 0xdebb9ec5L, 0x47b2cf7fL, 0x30b5ffe9L,
      0xbdbdf21cL, 0xcabac28aL, 0x53b39330L, 0x24b4a3a6L, 0xbad03605L, 0xcdd70693L, 0x54de5729L, 0x23d967bfL,
      0xb3667a2eL, 0xc4614ab8L, 0x5d681b02L, 0x2a6f2b94L, 0xb40bbe37L, 0xc30c8ea1L, 0x5a05df1bL, 0x2d02ef8dL};
  constexpr std::uint32_t operator()(string_view value) const noexcept {
    auto crc = static_cast<std::uint32_t>(0xffffffffL);
    for (const auto c : value) {
      crc = (crc >> 8) ^ crc_table[(crc ^ static_cast<std::uint32_t>(c)) & 0xff];
    }
    return crc ^ 0xffffffffL;
  }

  struct secondary_hash {
    constexpr std::uint32_t operator()(string_view value) const noexcept {
      auto acc = static_cast<std::uint64_t>(2166136261ULL);
      for (const auto c : value) {
        acc = ((acc ^ static_cast<std::uint64_t>(c)) * static_cast<std::uint64_t>(16777619ULL)) &
              (std::numeric_limits<std::uint32_t>::max)();
      }
      return static_cast<std::uint32_t>(acc);
    }
  };
};

template <typename Hash>
inline constexpr Hash hash_v{};

template <auto *GlobValues, typename Hash>
constexpr auto calculate_cases(std::size_t Page) noexcept {
  constexpr std::array values = *GlobValues;
  constexpr std::size_t size = values.size();

  using switch_t = std::invoke_result_t<Hash, typename decltype(values)::value_type>;
  static_assert(std::is_integral_v<switch_t> && !std::is_same_v<switch_t, bool>);
  const std::size_t values_to = (std::min)(static_cast<std::size_t>(256), size - Page);

  std::array<switch_t, 256> result{};
  auto fill = result.begin();
  {
    auto first = values.begin() + static_cast<std::ptrdiff_t>(Page);
    auto last = values.begin() + static_cast<std::ptrdiff_t>(Page + values_to);
    while (first != last) {
      *fill++ = hash_v<Hash>(*first++);
    }
  }

  // dead cases, try to avoid case collisions
  for (switch_t last_value = result[values_to - 1];
       fill != result.end() && last_value != (std::numeric_limits<switch_t>::max)();
       *fill++ = ++last_value) {
  }

  {
    auto it = result.begin();
    auto last_value = (std::numeric_limits<switch_t>::min)();
    for (; fill != result.end(); *fill++ = last_value++) {
      while (last_value == *it) {
        ++last_value, ++it;
      }
    }
  }

  return result;
}

template <typename R, typename F, typename... Args>
constexpr R invoke_r(F &&f, Args &&...args) noexcept(std::is_nothrow_invocable_r_v<R, F, Args...>) {
  if constexpr (std::is_void_v<R>) {
    std::forward<F>(f)(std::forward<Args>(args)...);
  } else {
    return static_cast<R>(std::forward<F>(f)(std::forward<Args>(args)...));
  }
}

enum class case_call_t { index, value };

template <typename T = void>
inline constexpr auto default_result_type_lambda =
    []() noexcept(std::is_nothrow_default_constructible_v<T>) { return T{}; };

template <>
inline constexpr auto default_result_type_lambda<void> = []() noexcept {};

template <auto *Arr, typename Hash>
constexpr bool has_duplicate() noexcept {
  using value_t = std::decay_t<decltype((*Arr)[0])>;
  using hash_value_t = std::invoke_result_t<Hash, value_t>;
  std::array<hash_value_t, Arr->size()> hashes{};
  std::size_t size = 0;
  for (auto elem : *Arr) {
    hashes[size] = hash_v<Hash>(elem);
    for (auto i = size++; i > 0; --i) {
      if (hashes[i] < hashes[i - 1]) {
        auto tmp = hashes[i];
        hashes[i] = hashes[i - 1];
        hashes[i - 1] = tmp;
      } else if (hashes[i] == hashes[i - 1]) {
        return false;
      } else {
        break;
      }
    }
  }
  return true;
}

#define MAGIC_ENUM_FOR_EACH_256(T) \
  T(0)                             \
  T(1)                             \
  T(2)                             \
  T(3)                             \
  T(4)                             \
  T(5)                             \
  T(6)                             \
  T(7)                             \
  T(8)                             \
  T(9)                             \
  T(10)                            \
  T(11)                            \
  T(12)                            \
  T(13)                            \
  T(14)                            \
  T(15)                            \
  T(16)                            \
  T(17)                            \
  T(18)                            \
  T(19)                            \
  T(20)                            \
  T(21)                            \
  T(22)                            \
  T(23)                            \
  T(24)                            \
  T(25)                            \
  T(26)                            \
  T(27)                            \
  T(28)                            \
  T(29)                            \
  T(30)                            \
  T(31)                            \
  T(32)                            \
  T(33)                            \
  T(34)                            \
  T(35)                            \
  T(36)                            \
  T(37)                            \
  T(38)                            \
  T(39)                            \
  T(40)                            \
  T(41)                            \
  T(42)                            \
  T(43)                            \
  T(44)                            \
  T(45)                            \
  T(46)                            \
  T(47)                            \
  T(48)                            \
  T(49)                            \
  T(50)                            \
  T(51)                            \
  T(52)                            \
  T(53)                            \
  T(54)                            \
  T(55)                            \
  T(56)                            \
  T(57)                            \
  T(58)                            \
  T(59)                            \
  T(60)                            \
  T(61)                            \
  T(62)                            \
  T(63)                            \
  T(64)                            \
  T(65)                            \
  T(66)                            \
  T(67)                            \
  T(68)                            \
  T(69)                            \
  T(70)                            \
  T(71)                            \
  T(72)                            \
  T(73)                            \
  T(74)                            \
  T(75)                            \
  T(76)                            \
  T(77)                            \
  T(78)                            \
  T(79)                            \
  T(80)                            \
  T(81)                            \
  T(82)                            \
  T(83)                            \
  T(84)                            \
  T(85)                            \
  T(86)                            \
  T(87)                            \
  T(88)                            \
  T(89)                            \
  T(90)                            \
  T(91)                            \
  T(92)                            \
  T(93)                            \
  T(94)                            \
  T(95)                            \
  T(96)                            \
  T(97)                            \
  T(98)                            \
  T(99)                            \
  T(100)                           \
  T(101)                           \
  T(102)                           \
  T(103)                           \
  T(104)                           \
  T(105)                           \
  T(106)                           \
  T(107)                           \
  T(108)                           \
  T(109)                           \
  T(110)                           \
  T(111)                           \
  T(112)                           \
  T(113)                           \
  T(114)                           \
  T(115)                           \
  T(116)                           \
  T(117)                           \
  T(118)                           \
  T(119)                           \
  T(120)                           \
  T(121)                           \
  T(122)                           \
  T(123)                           \
  T(124)                           \
  T(125)                           \
  T(126)                           \
  T(127)                           \
  T(128)                           \
  T(129)                           \
  T(130)                           \
  T(131)                           \
  T(132)                           \
  T(133)                           \
  T(134)                           \
  T(135)                           \
  T(136)                           \
  T(137)                           \
  T(138)                           \
  T(139)                           \
  T(140)                           \
  T(141)                           \
  T(142)                           \
  T(143)                           \
  T(144)                           \
  T(145)                           \
  T(146)                           \
  T(147)                           \
  T(148)                           \
  T(149)                           \
  T(150)                           \
  T(151)                           \
  T(152)                           \
  T(153)                           \
  T(154)                           \
  T(155)                           \
  T(156)                           \
  T(157)                           \
  T(158)                           \
  T(159)                           \
  T(160)                           \
  T(161)                           \
  T(162)                           \
  T(163)                           \
  T(164)                           \
  T(165)                           \
  T(166)                           \
  T(167)                           \
  T(168)                           \
  T(169)                           \
  T(170)                           \
  T(171)                           \
  T(172)                           \
  T(173)                           \
  T(174)                           \
  T(175)                           \
  T(176)                           \
  T(177)                           \
  T(178)                           \
  T(179)                           \
  T(180)                           \
  T(181)                           \
  T(182)                           \
  T(183)                           \
  T(184)                           \
  T(185)                           \
  T(186)                           \
  T(187)                           \
  T(188)                           \
  T(189)                           \
  T(190)                           \
  T(191)                           \
  T(192)                           \
  T(193)                           \
  T(194)                           \
  T(195)                           \
  T(196)                           \
  T(197)                           \
  T(198)                           \
  T(199)                           \
  T(200)                           \
  T(201)                           \
  T(202)                           \
  T(203)                           \
  T(204)                           \
  T(205)                           \
  T(206)                           \
  T(207)                           \
  T(208)                           \
  T(209)                           \
  T(210)                           \
  T(211)                           \
  T(212)                           \
  T(213)                           \
  T(214)                           \
  T(215)                           \
  T(216)                           \
  T(217)                           \
  T(218)                           \
  T(219)                           \
  T(220)                           \
  T(221)                           \
  T(222)                           \
  T(223)                           \
  T(224)                           \
  T(225)                           \
  T(226)                           \
  T(227)                           \
  T(228)                           \
  T(229)                           \
  T(230)                           \
  T(231)                           \
  T(232)                           \
  T(233)                           \
  T(234)                           \
  T(235)                           \
  T(236)                           \
  T(237)                           \
  T(238)                           \
  T(239) T(240) T(241) T(242) T(243) T(244) T(245) T(246) T(247) T(248) T(249) T(250) T(251) T(252) T(253) T(254) T(255)

#define MAGIC_ENUM_CASE(val)                                                                                      \
  case cases[val]:                                                                                                \
    if constexpr ((val) + Page < size) {                                                                          \
      if (!pred(values[val + Page], searched)) {                                                                  \
        break;                                                                                                    \
      }                                                                                                           \
      if constexpr (CallValue == case_call_t::index) {                                                            \
        if constexpr (std::is_invocable_r_v<result_t, Lambda, std::integral_constant<std::size_t, val + Page>>) { \
          return detail::invoke_r<result_t>(std::forward<Lambda>(lambda),                                         \
                                            std::integral_constant<std::size_t, val + Page>{});                   \
        } else if constexpr (std::is_invocable_v<Lambda, std::integral_constant<std::size_t, val + Page>>) {      \
          assert(false && "magic_enum::detail::constexpr_switch wrong result type.");                             \
        }                                                                                                         \
      } else if constexpr (CallValue == case_call_t::value) {                                                     \
        if constexpr (std::is_invocable_r_v<result_t, Lambda, enum_constant<values[val + Page]>>) {               \
          return detail::invoke_r<result_t>(std::forward<Lambda>(lambda), enum_constant<values[val + Page]>{});   \
        } else if constexpr (std::is_invocable_r_v<result_t, Lambda, enum_constant<values[val + Page]>>) {        \
          assert(false && "magic_enum::detail::constexpr_switch wrong result type.");                             \
        }                                                                                                         \
      }                                                                                                           \
      break;                                                                                                      \
    } else                                                                                                        \
      [[fallthrough]];

template <auto *GlobValues,
          case_call_t CallValue,
          std::size_t Page = 0,
          typename Hash = constexpr_hash_t<typename std::decay_t<decltype(*GlobValues)>::value_type>,
          typename BinaryPredicate = std::equal_to<>,
          typename Lambda,
          typename ResultGetterType>
constexpr decltype(auto) constexpr_switch(Lambda &&lambda,
                                          typename std::decay_t<decltype(*GlobValues)>::value_type searched,
                                          ResultGetterType &&def,
                                          BinaryPredicate &&pred = {}) {
  using result_t = std::invoke_result_t<ResultGetterType>;
  using hash_t = std::conditional_t<has_duplicate<GlobValues, Hash>(), Hash, typename Hash::secondary_hash>;
  static_assert(has_duplicate<GlobValues, hash_t>(),
                "magic_enum::detail::constexpr_switch duplicated hash found, please report it: "
                "https://github.com/Neargye/magic_enum/issues.");
  constexpr std::array values = *GlobValues;
  constexpr std::size_t size = values.size();
  constexpr std::array cases = calculate_cases<GlobValues, hash_t>(Page);

  switch (hash_v<hash_t>(searched)) {
    MAGIC_ENUM_FOR_EACH_256(MAGIC_ENUM_CASE)
    default:
      if constexpr (size > 256 + Page) {
        return constexpr_switch<GlobValues, CallValue, Page + 256, Hash>(std::forward<Lambda>(lambda),
                                                                         searched,
                                                                         std::forward<ResultGetterType>(def));
      }
      break;
  }
  return def();
}

#undef MAGIC_ENUM_FOR_EACH_256
#undef MAGIC_ENUM_CASE

#else
template <typename T>
inline constexpr bool has_hash = false;
#endif

template <typename E, typename F, std::size_t... I>
constexpr auto for_each(F &&f, std::index_sequence<I...>) {
  static_assert(is_enum_v<E>, "magic_enum::detail::for_each requires enum type.");
  constexpr bool has_void_return = (std::is_void_v<std::invoke_result_t<F, enum_constant<values_v<E>[I]>>> || ...);
  constexpr bool all_same_return = (std::is_same_v<std::invoke_result_t<F, enum_constant<values_v<E>[0]>>,
                                                   std::invoke_result_t<F, enum_constant<values_v<E>[I]>>> &&
                                    ...);

  if constexpr (has_void_return) {
    (f(enum_constant<values_v<E>[I]> {}), ...);
  } else if constexpr (all_same_return) {
    return std::array{f(enum_constant<values_v<E>[I]> {})...};
  } else {
    return std::tuple{f(enum_constant<values_v<E>[I]> {})...};
  }
}

template <typename E, typename F, std::size_t... I>
constexpr bool all_invocable(std::index_sequence<I...>) {
  static_assert(is_enum_v<E>, "magic_enum::detail::all_invocable requires enum type.");

  if constexpr (count_v<E> == 0) {
    return false;
  } else {
    return (std::is_invocable_v<F, enum_constant<values_v<E>[I]>> && ...);
  }
}

}  // namespace detail

// Checks is magic_enum supported compiler.
inline constexpr bool is_magic_enum_supported = detail::supported<void>::value;

template <typename T>
using Enum = detail::enum_concept<T>;

// Checks whether T is an Unscoped enumeration type.
// Provides the member constant value which is equal to true, if T is an [Unscoped
// enumeration](https://en.cppreference.com/w/cpp/language/enum#Unscoped_enumeration) type. Otherwise, value is equal to
// false.
template <typename T>
struct is_unscoped_enum : detail::is_unscoped_enum<T> {};

template <typename T>
inline constexpr bool is_unscoped_enum_v = is_unscoped_enum<T>::value;

// Checks whether T is an Scoped enumeration type.
// Provides the member constant value which is equal to true, if T is an [Scoped
// enumeration](https://en.cppreference.com/w/cpp/language/enum#Scoped_enumerations) type. Otherwise, value is equal to
// false.
template <typename T>
struct is_scoped_enum : detail::is_scoped_enum<T> {};

template <typename T>
inline constexpr bool is_scoped_enum_v = is_scoped_enum<T>::value;

// If T is a complete enumeration type, provides a member typedef type that names the underlying type of T.
// Otherwise, if T is not an enumeration type, there is no member type. Otherwise (T is an incomplete enumeration type),
// the program is ill-formed.
template <typename T>
struct underlying_type : detail::underlying_type<T> {};

template <typename T>
using underlying_type_t = typename underlying_type<T>::type;

template <auto V>
using enum_constant = detail::enum_constant<V>;

// Returns type name of enum.
template <typename E>
[[nodiscard]] constexpr auto enum_type_name() noexcept -> detail::enable_if_t<E, string_view> {
  constexpr string_view name = detail::type_name_v<std::decay_t<E>>;
  static_assert(!name.empty(), "magic_enum::enum_type_name enum type does not have a name.");

  return name;
}

// Returns number of enum values.
template <typename E>
[[nodiscard]] constexpr auto enum_count() noexcept -> detail::enable_if_t<E, std::size_t> {
  return detail::count_v<std::decay_t<E>>;
}

// Returns enum value at specified index.
// No bounds checking is performed: the behavior is undefined if index >= number of enum values.
template <typename E>
[[nodiscard]] constexpr auto enum_value(std::size_t index) noexcept -> detail::enable_if_t<E, std::decay_t<E>> {
  using D = std::decay_t<E>;

  if constexpr (detail::is_sparse_v<D>) {
    return assert((index < detail::count_v<D>)), detail::values_v<D>[index];
  } else {
    constexpr bool is_flag = detail::is_flags_v<D>;
    constexpr auto min = is_flag ? detail::log2(detail::min_v<D>) : detail::min_v<D>;

    return assert((index < detail::count_v<D>)), detail::value<D, min, is_flag>(index);
  }
}

// Returns enum value at specified index.
template <typename E, std::size_t I>
[[nodiscard]] constexpr auto enum_value() noexcept -> detail::enable_if_t<E, std::decay_t<E>> {
  using D = std::decay_t<E>;
  static_assert(I < detail::count_v<D>, "magic_enum::enum_value out of range.");

  return enum_value<D>(I);
}

// Returns std::array with enum values, sorted by enum value.
template <typename E>
[[nodiscard]] constexpr auto enum_values() noexcept -> detail::enable_if_t<E, detail::values_t<E>> {
  return detail::values_v<std::decay_t<E>>;
}

// Returns integer value from enum value.
template <typename E>
[[nodiscard]] constexpr auto enum_integer(E value) noexcept -> detail::enable_if_t<E, underlying_type_t<E>> {
  return static_cast<underlying_type_t<E>>(value);
}

// Returns underlying value from enum value.
template <typename E>
[[nodiscard]] constexpr auto enum_underlying(E value) noexcept -> detail::enable_if_t<E, underlying_type_t<E>> {
  return static_cast<underlying_type_t<E>>(value);
}

// Obtains index in enum values from enum value.
// Returns optional with index.
template <typename E>
[[nodiscard]] constexpr auto enum_index(E value) noexcept -> detail::enable_if_t<E, optional<std::size_t>> {
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;

  if constexpr (detail::count_v<D> == 0) {
    return {};  // Empty enum.
  } else if constexpr (detail::is_sparse_v<D> || detail::is_flags_v<D>) {
#if defined(MAGIC_ENUM_ENABLE_HASH)
    return detail::constexpr_switch<&detail::values_v<D>, detail::case_call_t::index>(
        [](std::size_t i) { return optional<std::size_t>{i}; },
        value,
        detail::default_result_type_lambda<optional<std::size_t>>);
#else
    for (std::size_t i = 0; i < detail::count_v<D>; ++i) {
      if (enum_value<D>(i) == value) {
        return i;
      }
    }
    return {};  // Invalid value or out of range.
#endif
  } else {
    const auto v = static_cast<U>(value);
    if (v >= detail::min_v<D> && v <= detail::max_v<D>) {
      return static_cast<std::size_t>(v - detail::min_v<D>);
    }
    return {};  // Invalid value or out of range.
  }
}

// Obtains index in enum values from static storage enum variable.
template <auto V>
[[nodiscard]] constexpr auto enum_index() noexcept -> detail::enable_if_t<decltype(V), std::size_t> {
  constexpr auto index = enum_index<std::decay_t<decltype(V)>>(V);
  static_assert(index, "magic_enum::enum_index enum value does not have a index.");

  return *index;
}

// Returns name from static storage enum variable.
// This version is much lighter on the compile times and is not restricted to the enum_range limitation.
template <auto V>
[[nodiscard]] constexpr auto enum_name() noexcept -> detail::enable_if_t<decltype(V), string_view> {
  constexpr string_view name = detail::enum_name_v<std::decay_t<decltype(V)>, V>;
  static_assert(!name.empty(), "magic_enum::enum_name enum value does not have a name.");

  return name;
}

// Returns name from enum value.
// If enum value does not have name or value out of range, returns empty string.
template <typename E, detail::value_type VT = detail::value_type::default_value>
[[nodiscard]] constexpr auto enum_name(E value) noexcept -> detail::enable_if_default_t<E, string_view, VT> {
  using D = std::decay_t<E>;

  if (const auto i = enum_index<D>(value)) {
    return detail::names_v<D>[*i];
  }
  return {};
}

// Returns name from enum value.
// If enum value does not have name or value out of range, returns empty string.
template <detail::value_type VT, typename E>
[[nodiscard]] constexpr auto enum_name(E value) -> detail::enable_if_default_t<E, string_view, VT> {
  using D = std::decay_t<E>;

  return enum_name<D, VT>(value);
}

// Returns name from enum value.
// If enum value does not have name or value out of range, returns empty string.
template <typename E, detail::value_type VT>
[[nodiscard]] auto enum_name(E value) -> detail::enable_if_flags_t<E, string, VT> {
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;
  static_assert(detail::is_flags_v<D>, "magic_enum::enum_flags_name requires enum-flags type.");

  string name;
  auto check_value = U{0};
  for (std::size_t i = 0; i < detail::count_v<D>; ++i) {
    if (const auto v = static_cast<U>(enum_value<D>(i)); (static_cast<U>(value) & v) != 0) {
      check_value |= v;
      const auto n = detail::names_v<D>[i];
      if (!name.empty()) {
        name.append(1, '|');
      }
      name.append(n.data(), n.size());
    }
  }

  if (check_value != 0 && check_value == static_cast<U>(value)) {
    return name;
  }
  return {};  // Invalid value or out of range.
}

// Returns name from enum value.
// If enum value does not have name or value out of range, returns empty string.
template <detail::value_type VT, typename E>
[[nodiscard]] auto enum_name(E value) -> detail::enable_if_flags_t<E, string, VT> {
  using D = std::decay_t<E>;
  static_assert(detail::is_flags_v<D>, "magic_enum::enum_flags_name requires enum-flags type.");

  return enum_name<D, VT>(value);
}

// Returns name from enum-flags value.
// If enum-flags value does not have name or value out of range, returns empty string.
template <typename E>
[[nodiscard]] auto enum_flags_name(E value) -> detail::enable_if_t<E, string> {
  using D = std::decay_t<E>;
  static_assert(detail::is_flags_v<D>, "magic_enum::enum_flags_name requires enum-flags type.");

  return enum_name<D, detail::value_type::flags_value>(value);
}

// Returns std::array with names, sorted by enum value.
template <typename E>
[[nodiscard]] constexpr auto enum_names() noexcept -> detail::enable_if_t<E, detail::names_t<E>> {
  return detail::names_v<std::decay_t<E>>;
}

// Returns std::array with pairs (value, name), sorted by enum value.
template <typename E>
[[nodiscard]] constexpr auto enum_entries() noexcept -> detail::enable_if_t<E, detail::entries_t<E>> {
  return detail::entries_v<std::decay_t<E>>;
}

// Allows you to write magic_enum::enum_cast<foo>("bar", magic_enum::case_insensitive);
inline constexpr auto case_insensitive = detail::case_insensitive{};

// Obtains enum value from integer value.
// Returns optional with enum value.
template <typename E, detail::value_type VT = detail::value_type::default_value>
[[nodiscard]] constexpr auto enum_cast(underlying_type_t<E> value) noexcept
    -> detail::enable_if_t<E, optional<std::decay_t<E>>> {
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;

  if constexpr (detail::count_v<D> == 0) {
    return {};  // Empty enum.
  } else if constexpr (VT == detail::value_type::flags_value && detail::is_flags_v<D>) {
    if constexpr (detail::is_sparse_v<D>) {
      auto check_value = U{0};
      for (std::size_t i = 0; i < detail::count_v<D>; ++i) {
        if (const auto v = static_cast<U>(enum_value<D>(i)); (value & v) != 0) {
          check_value |= v;
        }
      }

      if (check_value != 0 && check_value == value) {
        return static_cast<D>(value);
      }
    } else {
      constexpr auto min = detail::min_v<D>;
      constexpr auto max = detail::values_ors<D>();

      if (value >= min && value <= max) {
        return static_cast<D>(value);
      }
    }
    return {};  // Invalid value or out of range.
  } else {
#if defined(MAGIC_ENUM_ENABLE_HASH)
    return detail::constexpr_switch<&detail::values_v<D>, detail::case_call_t::value>(
        [](D v) { return optional<D>{v}; },
        static_cast<D>(value),
        detail::default_result_type_lambda<optional<D>>);
#else
    if constexpr (detail::is_sparse_v<D> || detail::is_flags_v<D>) {
      for (std::size_t i = 0; i < detail::count_v<D>; ++i) {
        if (value == static_cast<U>(enum_value<D>(i))) {
          return static_cast<D>(value);
        }
      }
    } else {
      if (value >= detail::min_v<D> && value <= detail::max_v<D>) {
        return static_cast<D>(value);
      }
    }
    return {};  // Invalid value or out of range.
#endif
  }
}

// Obtains enum-flags value from integer value.
// Returns optional with enum-flags value.
template <typename E>
[[nodiscard]] constexpr auto enum_flags_cast(underlying_type_t<E> value) noexcept
    -> detail::enable_if_t<E, optional<std::decay_t<E>>> {
  using D = std::decay_t<E>;
  static_assert(detail::is_flags_v<D>, "magic_enum::enum_flags_cast requires enum-flags type.");

  return enum_cast<D, detail::value_type::flags_value>(value);
}

// Obtains enum value from name.
// Returns optional with enum value.
template <typename E,
          detail::value_type VT = detail::value_type::default_value,
          typename BinaryPredicate = std::equal_to<>>
[[nodiscard]] constexpr auto enum_cast(string_view value, [[maybe_unused]] BinaryPredicate p = {}) noexcept(
    detail::is_nothrow_invocable<BinaryPredicate>())
    -> detail::enable_if_t<E, optional<std::decay_t<E>>, BinaryPredicate> {
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;

  if constexpr (detail::count_v<D> == 0) {
    return {};  // Empty enum.
  } else if constexpr (VT == detail::value_type::flags_value && detail::is_flags_v<D>) {
    auto result = U{0};
    while (!value.empty()) {
      const auto d = detail::find(value, '|');
      const auto s = (d == string_view::npos) ? value : value.substr(0, d);
      auto f = U{0};
      for (std::size_t i = 0; i < detail::count_v<D>; ++i) {
        if (detail::cmp_equal(s, detail::names_v<D>[i], p)) {
          f = static_cast<U>(enum_value<D>(i));
          result |= f;
          break;
        }
      }
      if (f == U{0}) {
        return {};  // Invalid value or out of range.
      }
      value.remove_prefix((d == string_view::npos) ? value.size() : d + 1);
    }

    if (result != U{0}) {
      return static_cast<D>(result);
    }
    return {};  // Invalid value or out of range.
  } else {
    if constexpr (detail::is_default_predicate<BinaryPredicate>() && detail::has_hash<D>) {
#if defined(MAGIC_ENUM_ENABLE_HASH)
      return detail::constexpr_switch<&detail::names_v<D>, detail::case_call_t::index>(
          [](std::size_t i) { return optional<D>{detail::values_v<D>[i]}; },
          value,
          detail::default_result_type_lambda<optional<D>>,
          [&p](string_view lhs, string_view rhs) { return detail::cmp_equal(lhs, rhs, p); });
#endif
    } else {
      for (std::size_t i = 0; i < detail::count_v<D>; ++i) {
        if (detail::cmp_equal(value, detail::names_v<D>[i], p)) {
          return enum_value<D>(i);
        }
      }
      return {};  // Invalid value or out of range.
    }
  }
}

// Obtains enum-flags value from name.
// Returns optional with enum-flags value.
template <typename E, typename BinaryPredicate = std::equal_to<>>
[[nodiscard]] constexpr auto enum_flags_cast(string_view value, [[maybe_unused]] BinaryPredicate p = {}) noexcept(
    detail::is_nothrow_invocable<BinaryPredicate>())
    -> detail::enable_if_t<E, optional<std::decay_t<E>>, BinaryPredicate> {
  using D = std::decay_t<E>;
  static_assert(detail::is_flags_v<D>, "magic_enum::enum_flags_cast requires enum-flags type.");

  return enum_cast<D, detail::value_type::flags_value>(value, std::move(p));
}

// Checks whether enum contains value with such value.
template <typename E, detail::value_type VT = detail::value_type::default_value>
[[nodiscard]] constexpr auto enum_contains(E value) noexcept -> detail::enable_if_t<E, bool> {
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;

  return static_cast<bool>(enum_cast<D, VT>(static_cast<U>(value)));
}

// Checks whether enum-flags contains value with such value.
template <typename E>
[[nodiscard]] constexpr auto enum_flags_contains(E value) noexcept -> detail::enable_if_t<E, bool> {
  using D = std::decay_t<E>;
  static_assert(detail::is_flags_v<D>, "magic_enum::enum_flags_contains requires enum-flags type.");

  return enum_contains<D, detail::value_type::flags_value>(value);
}

// Checks whether enum contains value with such integer value.
template <typename E, detail::value_type VT = detail::value_type::default_value>
[[nodiscard]] constexpr auto enum_contains(underlying_type_t<E> value) noexcept -> detail::enable_if_t<E, bool> {
  using D = std::decay_t<E>;

  return static_cast<bool>(enum_cast<D, VT>(value));
}

// Checks whether enum-flags contains value with such integer value.
template <typename E>
[[nodiscard]] constexpr auto enum_flags_contains(underlying_type_t<E> value) noexcept -> detail::enable_if_t<E, bool> {
  using D = std::decay_t<E>;
  static_assert(detail::is_flags_v<D>, "magic_enum::enum_flags_contains requires enum-flags type.");

  return enum_contains<D, detail::value_type::flags_value>(value);
}

// Checks whether enum contains enumerator with such name.
template <typename E,
          detail::value_type VT = detail::value_type::default_value,
          typename BinaryPredicate = std::equal_to<>>
[[nodiscard]] constexpr auto enum_contains(string_view value, BinaryPredicate p = {}) noexcept(
    detail::is_nothrow_invocable<BinaryPredicate>()) -> detail::enable_if_t<E, bool, BinaryPredicate> {
  using D = std::decay_t<E>;

  return static_cast<bool>(enum_cast<D, VT>(value, std::move(p)));
}

// Checks whether enum-flags contains enumerator with such name.
template <typename E, typename BinaryPredicate = std::equal_to<>>
[[nodiscard]] constexpr auto enum_flags_contains(string_view value, BinaryPredicate p = {}) noexcept(
    detail::is_nothrow_invocable<BinaryPredicate>()) -> detail::enable_if_t<E, bool, BinaryPredicate> {
  using D = std::decay_t<E>;
  static_assert(detail::is_flags_v<D>, "magic_enum::enum_flags_contains requires enum-flags type.");

  return enum_contains<D, detail::value_type::flags_value>(value, std::move(p));
}

template <typename E, typename F, detail::enable_if_t<E, int> = 0>
constexpr auto enum_for_each(F &&f) {
  using D = std::decay_t<E>;
  static_assert(std::is_enum_v<D>, "magic_enum::enum_for_each requires enum type.");
  constexpr auto sep = std::make_index_sequence<detail::count_v<D>>{};

  if constexpr (detail::all_invocable<D, F>(sep)) {
    return detail::for_each<D>(std::forward<F>(f), sep);
  } else {
    static_assert(detail::always_false_v<D>, "magic_enum::enum_for_each requires invocable of all enum value.");
  }
}

template <bool AsFlags = true>
inline constexpr auto as_flags = AsFlags ? detail::value_type::flags_value : detail::value_type::default_value;

#if !defined(MAGIC_ENUM_NO_STREAMS)

namespace ostream_operators {

template <typename Char, typename Traits, typename E, detail::enable_if_t<E, int> = 0>
std::basic_ostream<Char, Traits> &operator<<(std::basic_ostream<Char, Traits> &os, E value) {
  using D = std::decay_t<E>;
  using U = underlying_type_t<D>;

  if constexpr (detail::supported<D>::value) {
    if (const auto name = enum_name<D, as_flags<detail::is_flags_v<D>>>(value); !name.empty()) {
      for (const auto c : name) {
        os.put(c);
      }
      return os;
    }
  }
  return (os << static_cast<U>(value));
}

template <typename Char, typename Traits, typename E, detail::enable_if_t<E, int> = 0>
std::basic_ostream<Char, Traits> &operator<<(std::basic_ostream<Char, Traits> &os, optional<E> value) {
  return value ? (os << *value) : os;
}

}  // namespace ostream_operators

namespace istream_operators {

template <typename Char, typename Traits, typename E, detail::enable_if_t<E, int> = 0>
std::basic_istream<Char, Traits> &operator>>(std::basic_istream<Char, Traits> &is, E &value) {
  using D = std::decay_t<E>;

  std::basic_string<Char, Traits> s;
  is >> s;
  if (const auto v = enum_cast<D, as_flags<detail::is_flags_v<D>>>(s)) {
    value = *v;
  } else {
    is.setstate(std::basic_ios<Char>::failbit);
  }
  return is;
}

}  // namespace istream_operators

namespace iostream_operators {

using namespace ostream_operators;
using namespace istream_operators;

}  // namespace iostream_operators

#endif

namespace bitwise_operators {

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E operator~(E rhs) noexcept {
  return static_cast<E>(~static_cast<underlying_type_t<E>>(rhs));
}

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E operator|(E lhs, E rhs) noexcept {
  return static_cast<E>(static_cast<underlying_type_t<E>>(lhs) | static_cast<underlying_type_t<E>>(rhs));
}

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E operator&(E lhs, E rhs) noexcept {
  return static_cast<E>(static_cast<underlying_type_t<E>>(lhs) & static_cast<underlying_type_t<E>>(rhs));
}

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E operator^(E lhs, E rhs) noexcept {
  return static_cast<E>(static_cast<underlying_type_t<E>>(lhs) ^ static_cast<underlying_type_t<E>>(rhs));
}

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E &operator|=(E &lhs, E rhs) noexcept {
  return lhs = (lhs | rhs);
}

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E &operator&=(E &lhs, E rhs) noexcept {
  return lhs = (lhs & rhs);
}

template <typename E, detail::enable_if_t<E, int> = 0>
constexpr E &operator^=(E &lhs, E rhs) noexcept {
  return lhs = (lhs ^ rhs);
}

}  // namespace bitwise_operators

}  // namespace magic_enum

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif  // NEARGYE_MAGIC_ENUM_HPP
