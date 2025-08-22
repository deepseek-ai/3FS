#pragma once

#include <folly/sorted_vector_types.h>
#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "common/utils/RobinHood.h"

namespace hf3fs {

template <class T, template <class...> class Primary>
struct is_specialization_of : std::false_type {};

template <template <class...> class Primary, class... Args>
struct is_specialization_of<Primary<Args...>, Primary> : std::true_type {};

template <class T, template <class...> class Primary>
inline constexpr bool is_specialization_of_v = is_specialization_of<T, Primary>::value;

template <typename T, template <class...> class Primary>
concept is_specialization = is_specialization_of_v<T, Primary>;

template <class T>
inline constexpr bool is_vector_v = is_specialization_of_v<T, std::vector>;

template <class T>
inline constexpr bool is_set_v = is_specialization_of_v<T, std::set> || is_specialization_of_v<T, std::unordered_set>;

template <class T>
inline constexpr bool is_map_v = is_specialization_of_v<T, std::map> || is_specialization_of_v<T, std::unordered_map> ||
                                 is_specialization_of_v<T, folly::sorted_vector_map>;

template <typename Key, typename Hash, typename KeyEqual, size_t MaxLoad>
inline constexpr bool is_set_v<robin_hood::unordered_set<Key, Hash, KeyEqual, MaxLoad>> = true;

template <typename Key, typename T, typename Hash, typename KeyEqual, size_t MaxLoad>
inline constexpr bool is_map_v<robin_hood::unordered_map<Key, T, Hash, KeyEqual, MaxLoad>> = true;

template <class T>
inline constexpr bool is_bounded_array_v = std::is_bounded_array_v<T>;

template <class T, size_t N>
inline constexpr bool is_bounded_array_v<std::array<T, N>> = true;

template <typename T>
inline constexpr bool is_linear_container_v = is_bounded_array_v<T> || is_vector_v<T> || is_set_v<T>;

template <class T>
inline constexpr bool is_optional_v = is_specialization_of_v<T, std::optional>;

template <class T>
inline constexpr bool is_unique_ptr_v = is_specialization_of_v<T, std::unique_ptr>;

template <class T>
inline constexpr bool is_shared_ptr_v = is_specialization_of_v<T, std::shared_ptr>;

template <class T>
inline constexpr bool is_variant_v = is_specialization_of_v<T, std::variant>;

template <class T>
inline constexpr bool is_pair_v = is_specialization_of_v<T, std::pair>;

template <class T>
struct type_list;
template <class... Ts>
struct type_list<std::variant<Ts...>> {
  using type = std::tuple<Ts...>;
};
template <class T>
using type_list_t = typename type_list<T>::type;

template <class Ret, class... Args>
inline std::tuple<Args...> getFunctionParametersList(Ret (*)(Args...));
template <class T, class Ret, class... Args>
inline std::tuple<Args...> getFunctionParametersList(Ret (T::*)(Args...));
template <class T, class Ret, class... Args>
inline std::tuple<Args...> getFunctionParametersList(Ret (T::*)(Args...) const);
template <auto func>
using function_parameters_t = decltype(getFunctionParametersList(func));
template <auto func>
using function_first_parameter_t = std::decay_t<std::tuple_element_t<0, function_parameters_t<func>>>;

inline auto &getFirstParameter(auto &&first, auto &&...) { return first; }

template <class T, class M>
inline constexpr auto MemberPointerToClass(M T::*) -> T;
template <auto Pointer>
using member_pointer_to_class_t = decltype(MemberPointerToClass(Pointer));

template <class T>
concept GenericPair = requires {
  requires(std::is_same_v<typename T::first_type, decltype(T().first)>);
  requires(std::is_same_v<typename T::second_type, decltype(T().second)>);
};
template <class T>
inline constexpr bool is_generic_pair_v = GenericPair<T>;

template <class T>
concept Container = requires(T a, const T b) {
  requires std::regular<T>;
  requires std::swappable<T>;
  requires std::destructible<typename T::value_type>;
  requires std::forward_iterator<typename T::iterator>;
  requires std::forward_iterator<typename T::const_iterator>;
  { a.begin() } -> std::same_as<typename T::iterator>;
  { a.end() } -> std::same_as<typename T::iterator>;
  { b.begin() } -> std::same_as<typename T::const_iterator>;
  { b.end() } -> std::same_as<typename T::const_iterator>;
  { a.cbegin() } -> std::same_as<typename T::const_iterator>;
  { a.cend() } -> std::same_as<typename T::const_iterator>;
  { a.size() } -> std::same_as<typename T::size_type>;
  { a.max_size() } -> std::same_as<typename T::size_type>;
  { a.empty() } -> std::convertible_to<bool>;
};

template <auto Value>
struct value_identity {
  using type = decltype(Value);
  constexpr static auto value = Value;
};

template <class T>
struct enable_shared_from_this : public std::enable_shared_from_this<T> {
  template <class... Args>
  static std::shared_ptr<T> create(Args &&...args) {
    struct Impl : public T {
      Impl(Args &&...args)
          : T(std::forward<Args>(args)...) {}
    };
    static_assert(sizeof(Impl) == sizeof(T));
    return std::make_shared<Impl>(std::forward<Args>(args)...);
  }
};

template <class T>
constexpr auto callByIdx(auto &&f, size_t idx) {
  switch (idx) {
    case 0:
      if constexpr (0 < std::tuple_size_v<T>) return f(std::tuple_element_t<0, T>{});
    case 1:
      if constexpr (1 < std::tuple_size_v<T>) return f(std::tuple_element_t<1, T>{});
    case 2:
      if constexpr (2 < std::tuple_size_v<T>) return f(std::tuple_element_t<2, T>{});
    case 3:
      if constexpr (3 < std::tuple_size_v<T>) return f(std::tuple_element_t<3, T>{});
    case 4:
      if constexpr (4 < std::tuple_size_v<T>) return f(std::tuple_element_t<4, T>{});
    case 5:
      if constexpr (5 < std::tuple_size_v<T>) return f(std::tuple_element_t<5, T>{});
    case 6:
      if constexpr (6 < std::tuple_size_v<T>) return f(std::tuple_element_t<6, T>{});
    case 7:
      if constexpr (7 < std::tuple_size_v<T>) return f(std::tuple_element_t<7, T>{});
    case 8:
      if constexpr (8 < std::tuple_size_v<T>) return f(std::tuple_element_t<8, T>{});
    case 9:
      if constexpr (9 < std::tuple_size_v<T>) return f(std::tuple_element_t<9, T>{});
    case 10:
      if constexpr (10 < std::tuple_size_v<T>) return f(std::tuple_element_t<10, T>{});
    case 11:
      if constexpr (11 < std::tuple_size_v<T>) return f(std::tuple_element_t<11, T>{});
    case 12:
      if constexpr (12 < std::tuple_size_v<T>) return f(std::tuple_element_t<12, T>{});
    case 13:
      if constexpr (13 < std::tuple_size_v<T>) return f(std::tuple_element_t<13, T>{});
    case 14:
      if constexpr (14 < std::tuple_size_v<T>) return f(std::tuple_element_t<14, T>{});
    case 15:
      if constexpr (15 < std::tuple_size_v<T>) return f(std::tuple_element_t<15, T>{});
    case 16:
      if constexpr (16 < std::tuple_size_v<T>) return f(std::tuple_element_t<16, T>{});
    case 17:
      if constexpr (17 < std::tuple_size_v<T>) return f(std::tuple_element_t<17, T>{});
    case 18:
      if constexpr (18 < std::tuple_size_v<T>) return f(std::tuple_element_t<18, T>{});
    case 19:
      if constexpr (19 < std::tuple_size_v<T>) return f(std::tuple_element_t<19, T>{});
    case 20:
      if constexpr (20 < std::tuple_size_v<T>) return f(std::tuple_element_t<20, T>{});
    case 21:
      if constexpr (21 < std::tuple_size_v<T>) return f(std::tuple_element_t<21, T>{});
    case 22:
      if constexpr (22 < std::tuple_size_v<T>) return f(std::tuple_element_t<22, T>{});
    case 23:
      if constexpr (23 < std::tuple_size_v<T>) return f(std::tuple_element_t<23, T>{});
    case 24:
      if constexpr (24 < std::tuple_size_v<T>) return f(std::tuple_element_t<24, T>{});
    case 25:
      if constexpr (25 < std::tuple_size_v<T>) return f(std::tuple_element_t<25, T>{});
    case 26:
      if constexpr (26 < std::tuple_size_v<T>) return f(std::tuple_element_t<26, T>{});
    case 27:
      if constexpr (27 < std::tuple_size_v<T>) return f(std::tuple_element_t<27, T>{});
    case 28:
      if constexpr (28 < std::tuple_size_v<T>) return f(std::tuple_element_t<28, T>{});
    case 29:
      if constexpr (29 < std::tuple_size_v<T>) return f(std::tuple_element_t<29, T>{});
    case 30:
      if constexpr (30 < std::tuple_size_v<T>) return f(std::tuple_element_t<30, T>{});
    case 31:
      if constexpr (31 < std::tuple_size_v<T>) return f(std::tuple_element_t<31, T>{});
    case 32:
      if constexpr (32 < std::tuple_size_v<T>) return f(std::tuple_element_t<32, T>{});
    case 33:
      if constexpr (33 < std::tuple_size_v<T>) return f(std::tuple_element_t<33, T>{});
    case 34:
      if constexpr (34 < std::tuple_size_v<T>) return f(std::tuple_element_t<34, T>{});
    case 35:
      if constexpr (35 < std::tuple_size_v<T>) return f(std::tuple_element_t<35, T>{});
    case 36:
      if constexpr (36 < std::tuple_size_v<T>) return f(std::tuple_element_t<36, T>{});
    case 37:
      if constexpr (37 < std::tuple_size_v<T>) return f(std::tuple_element_t<37, T>{});
    case 38:
      if constexpr (38 < std::tuple_size_v<T>) return f(std::tuple_element_t<38, T>{});
    case 39:
      if constexpr (39 < std::tuple_size_v<T>) return f(std::tuple_element_t<39, T>{});
    case 40:
      if constexpr (40 < std::tuple_size_v<T>) return f(std::tuple_element_t<40, T>{});
    case 41:
      if constexpr (41 < std::tuple_size_v<T>) return f(std::tuple_element_t<41, T>{});
    case 42:
      if constexpr (42 < std::tuple_size_v<T>) return f(std::tuple_element_t<42, T>{});
    case 43:
      if constexpr (43 < std::tuple_size_v<T>) return f(std::tuple_element_t<43, T>{});
    case 44:
      if constexpr (44 < std::tuple_size_v<T>) return f(std::tuple_element_t<44, T>{});
    case 45:
      if constexpr (45 < std::tuple_size_v<T>) return f(std::tuple_element_t<45, T>{});
    case 46:
      if constexpr (46 < std::tuple_size_v<T>) return f(std::tuple_element_t<46, T>{});
    case 47:
      if constexpr (47 < std::tuple_size_v<T>) return f(std::tuple_element_t<47, T>{});
    case 48:
      if constexpr (48 < std::tuple_size_v<T>) return f(std::tuple_element_t<48, T>{});
    case 49:
      if constexpr (49 < std::tuple_size_v<T>) return f(std::tuple_element_t<49, T>{});
    case 50:
      if constexpr (50 < std::tuple_size_v<T>) return f(std::tuple_element_t<50, T>{});
    case 51:
      if constexpr (51 < std::tuple_size_v<T>) return f(std::tuple_element_t<51, T>{});
    case 52:
      if constexpr (52 < std::tuple_size_v<T>) return f(std::tuple_element_t<52, T>{});
    case 53:
      if constexpr (53 < std::tuple_size_v<T>) return f(std::tuple_element_t<53, T>{});
    case 54:
      if constexpr (54 < std::tuple_size_v<T>) return f(std::tuple_element_t<54, T>{});
    case 55:
      if constexpr (55 < std::tuple_size_v<T>) return f(std::tuple_element_t<55, T>{});
    case 56:
      if constexpr (56 < std::tuple_size_v<T>) return f(std::tuple_element_t<56, T>{});
    case 57:
      if constexpr (57 < std::tuple_size_v<T>) return f(std::tuple_element_t<57, T>{});
    case 58:
      if constexpr (58 < std::tuple_size_v<T>) return f(std::tuple_element_t<58, T>{});
    case 59:
      if constexpr (59 < std::tuple_size_v<T>) return f(std::tuple_element_t<59, T>{});
    case 60:
      if constexpr (60 < std::tuple_size_v<T>) return f(std::tuple_element_t<60, T>{});
    case 61:
      if constexpr (61 < std::tuple_size_v<T>) return f(std::tuple_element_t<61, T>{});
    case 62:
      if constexpr (62 < std::tuple_size_v<T>) return f(std::tuple_element_t<62, T>{});
    case 63:
      if constexpr (63 < std::tuple_size_v<T>) return f(std::tuple_element_t<63, T>{});
    default:
      return f(nullptr);
  }
}

}  // namespace hf3fs
