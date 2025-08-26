#pragma once

#include <algorithm>
#include <folly/Likely.h>
#include <iterator>
#include <limits>
#include <scn/scn.h>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "common/net/Allocator.h"
#include "common/serde/TypeName.h"
#include "common/utils/DownwardBytes.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/NameWrapper.h"
#include "common/utils/Path.h"
#include "common/utils/Reflection.h"
#include "common/utils/Result.h"
#include "common/utils/Thief.h"
#include "common/utils/TypeTraits.h"
#include "common/utils/Varint32.h"
#include "common/utils/Varint64.h"

#define SERDE_CLASS_TYPED_FIELD(TYPE, NAME, DEFAULT, ...)                                                          \
 private:                                                                                                          \
  friend struct ::hf3fs::refl::Helper;                                                                             \
  struct T##NAME : std::type_identity<REFL_NOW> {};                                                                \
  TYPE NAME##_ = DEFAULT;                                                                                          \
                                                                                                                   \
 public:                                                                                                           \
  auto &NAME() { return NAME##_; }                                                                                 \
  const auto &NAME() const { return NAME##_; }                                                                     \
                                                                                                                   \
 protected:                                                                                                        \
  constexpr auto T##NAME()->::hf3fs::thief::steal<struct T##NAME, std::decay_t<decltype(*this)>>;                  \
  REFL_ADD_SAFE(                                                                                                   \
      (::hf3fs::serde::FieldInfo<#NAME,                                                                            \
                                 &::hf3fs::thief::retrieve<struct T##NAME>::NAME##_ __VA_OPT__(, __VA_ARGS__)>{}), \
      typename T##NAME::type)

#define SERDE_STRUCT_TYPED_FIELD(TYPE, NAME, DEFAULT, ...)                                                             \
 private:                                                                                                              \
  friend struct ::hf3fs::refl::Helper;                                                                                 \
  struct T##NAME : std::type_identity<REFL_NOW> {};                                                                    \
                                                                                                                       \
 public:                                                                                                               \
  TYPE NAME = DEFAULT;                                                                                                 \
                                                                                                                       \
 protected:                                                                                                            \
  constexpr auto T##NAME()->::hf3fs::thief::steal<struct T##NAME, std::decay_t<decltype(*this)>>;                      \
  REFL_ADD_SAFE(                                                                                                       \
      (::hf3fs::serde::FieldInfo<#NAME, &::hf3fs::thief::retrieve<struct T##NAME>::NAME __VA_OPT__(, __VA_ARGS__)>{}), \
      typename T##NAME::type)

#define SERDE_CLASS_FIELD(NAME, DEFAULT, ...) \
  SERDE_CLASS_TYPED_FIELD(std::decay_t<decltype(DEFAULT)>, NAME, DEFAULT __VA_OPT__(, __VA_ARGS__))

#define SERDE_STRUCT_FIELD(NAME, DEFAULT, ...) \
  SERDE_STRUCT_TYPED_FIELD(std::decay_t<decltype(DEFAULT)>, NAME, DEFAULT __VA_OPT__(, __VA_ARGS__))

namespace hf3fs::serde {

template <NameWrapper Name, auto Getter, auto Checker = nullptr>
struct FieldInfo {
  static constexpr std::string_view name = Name;
  static constexpr auto getter = Getter;
  static constexpr auto checker = Checker;
};

template <auto... Args>
constexpr inline std::true_type is_field_info(FieldInfo<Args...>);
template <class T>
constexpr inline bool is_field_infos = false;
template <class... Ts>
constexpr inline bool is_field_infos<std::tuple<Ts...>> = (requires { is_field_info(std::declval<Ts>()); } && ...);

template <typename T>
concept SerdeType = bool(refl::Helper::Size<T>) && is_field_infos<refl::Helper::FieldInfoList<T>>;

template <typename T>
concept SerdeCopyable =
    std::is_same_v<T, bool> || std::is_integral_v<T> || std::is_floating_point_v<T> || std::is_enum_v<T> || requires {
  typename T::is_serde_copyable;
};

template <SerdeType T>
constexpr inline auto count() {
  return refl::Helper::Size<T>;
}
template <SerdeType T, size_t Index>
constexpr inline auto name() {
  return refl::Helper::FieldInfo<T, Index>::name;
}
template <SerdeType T, size_t Index>
constexpr inline auto getter() {
  return refl::Helper::FieldInfo<T, Index>::getter;
}
constexpr inline auto count(auto &&o) { return count<std::decay_t<decltype(o)>>(); }
template <size_t Index>
constexpr inline auto name(auto &&o) {
  return name<std::decay_t<decltype(o)>, Index>();
}
template <size_t Index>
constexpr inline auto &value(auto &&o) {
  return o.*getter<std::decay_t<decltype(o)>, Index>();
}

template <size_t I = 0>
constexpr inline auto iterate(auto &&f, auto &&o, auto &&...args) {
  return refl::Helper::iterate<std::decay_t<decltype(o)>>(
      [&](auto type) { return f(type.name, o.*type.getter, args...); });
}

template <class T>
struct SerdeMethod {
  static auto serialize(const T &o, auto &out) = delete;
  static auto serializeReadable(const T &o, auto &out) = delete;
  static auto deserialize(T &o, auto &in) = delete;
  static Result<T> deserializeReadable(T &o, auto &out) = delete;

  static auto serdeTo(const T &t) = delete;
  static auto serdeToReadable(const T &o) = delete;
  static Result<T> serdeFrom(auto) = delete;
  static Result<T> serdeFromReadable(auto) = delete;
};

template <typename T>
struct DefaultConstructor {
  static T construct() { return T{}; }
};

template <>
struct DefaultConstructor<Status> {
  static Status construct() { return Status::OK; }
};

template <typename T>
struct DefaultConstructor<Result<T>> {
  static Result<T> construct() { return T{}; }
};
}  // namespace hf3fs::serde

template <>
struct hf3fs::serde::SerdeMethod<hf3fs::Varint32> {
  static auto serialize(const Varint32 &o, auto &out) {
    constexpr int B = 128;
    uint8_t buf[5];
    auto ptr = buf;
    if (o < (1 << 7)) {
      *(ptr++) = o;
    } else if (o < (1 << 14)) {
      *(ptr++) = o | B;
      *(ptr++) = o >> 7;
    } else if (o < (1 << 21)) {
      *(ptr++) = o | B;
      *(ptr++) = (o >> 7) | B;
      *(ptr++) = o >> 14;
    } else if (o < (1 << 28)) {
      *(ptr++) = o | B;
      *(ptr++) = (o >> 7) | B;
      *(ptr++) = (o >> 14) | B;
      *(ptr++) = o >> 21;
    } else {
      *(ptr++) = o | B;
      *(ptr++) = (o >> 7) | B;
      *(ptr++) = (o >> 14) | B;
      *(ptr++) = (o >> 21) | B;
      *(ptr++) = o >> 28;
    }
    out.append(buf, ptr - buf);
  }

  static Result<Void> deserialize(Varint32 &o, auto &in) {
    o.value = 0;
    for (uint32_t shift = 0; shift <= 28 && !in.str().empty(); shift += 7) {
      uint32_t byte = static_cast<uint8_t>(in.str()[0]);
      in.str().remove_prefix(1);
      if (byte & 128) {
        // More bytes are present
        o.value |= ((byte & 127) << shift);
      } else {
        o.value |= (byte << shift);
        return Void{};
      }
    }
    return makeError(StatusCode::kSerdeInsufficientLength, fmt::format("varint32 is short"));
  }

  static uint32_t serdeToReadable(Varint32 o) { return o; }
};

template <>
struct hf3fs::serde::SerdeMethod<hf3fs::Varint64> {
  static auto serialize(const Varint64 &o, auto &out) {
    static const int B = 128;
    uint8_t buf[10];
    auto ptr = reinterpret_cast<uint8_t *>(buf);

    uint64_t v = o;
    while (v >= B) {
      *(ptr++) = v | B;
      v >>= 7;
    }
    *(ptr++) = static_cast<uint8_t>(v);
    out.append(buf, ptr - buf);
  }

  static Result<Void> deserialize(Varint64 &o, auto &in) {
    o.value = 0;
    for (uint32_t shift = 0; shift <= 63 && !in.str().empty(); shift += 7) {
      uint64_t byte = static_cast<uint8_t>(in.str()[0]);
      in.str().remove_prefix(1);
      if (byte & 128) {
        // More bytes are present
        o.value |= ((byte & 127) << shift);
      } else {
        o.value |= (byte << shift);
        return Void{};
      }
    }
    return makeError(StatusCode::kSerdeInsufficientLength, fmt::format("varint64 is short"));
  }

  static uint64_t serdeToReadable(Varint64 o) { return o; }
};

namespace hf3fs::serde {

enum class Optional : uint8_t { NullOpt, HasValue };
struct VariantIndex {
  uint8_t index;
};

template <class T>
class Out {
 public:
  auto tableBegin(bool isInline);  // begin a table.
  void tableEnd(auto);             // end a table.
  void arrayBegin();               // begin an array.
  void arrayEnd(uint32_t size);    // end an array.
  void key(std::string_view key);  // prepare the key in a key-value pair.
  void value(auto &&value);        // prepare the value in a key-value pair or an array.
};

template <class T>
class In {
 public:
  auto parseKey(std::string_view);
  auto parseTable();

  // for binary.
  Result<Void> parseCopyable(auto &o);

  // for Json/Toml.
  Result<bool> parseBoolean();
  Result<int64_t> parseInteger();
  Result<double> parseFloat();

  // for both.
  Result<std::string_view> parseString();
  Result<Void> parseOptional(Optional &optional);
  Result<std::pair<std::string_view, In>> parseVariant();
};

template <class O>
inline void serialize(auto &&o, Out<O> &out) {
  using T = std::decay_t<decltype(o)>;
  constexpr bool isBinaryOut = requires { typename Out<O>::is_binary_out; };
  if constexpr (requires { o.serdeToReadable(); } && !isBinaryOut) {
    serialize(o.serdeToReadable(), out);
  } else if constexpr (requires { SerdeMethod<T>::serializeReadable(o, out); } && !isBinaryOut) {
    SerdeMethod<T>::serializeReadable(o, out);
  } else if constexpr (requires { SerdeMethod<T>::serdeToReadable(o); } && !isBinaryOut) {
    serialize(SerdeMethod<T>::serdeToReadable(o), out);
  } else if constexpr (requires { SerdeMethod<T>::serialize(o, out); }) {
    static_assert(requires(T t, In<std::string_view> in) { Result<Void>{SerdeMethod<T>::deserialize(t, in)}; });
    SerdeMethod<T>::serialize(o, out);
  } else if constexpr (requires { SerdeMethod<T>::serdeTo(o); }) {
    static_assert(requires { SerdeMethod<T>::serdeFrom(SerdeMethod<T>::serdeTo(o)); });
    serialize(SerdeMethod<T>::serdeTo(o), out);
  } else if constexpr (SerdeType<T>) {
    auto start = out.tableBegin(false);
    if constexpr (isBinaryOut) {
      refl::Helper::iterate<T, true>([&](auto type) { serialize(o.*type.getter, out); },
                                     [&] { out.tableEnd(start), start = out.tableBegin(false); });
    } else {
      refl::Helper::iterate<T>([&](auto type) { out.key(type.name), serialize(o.*type.getter, out); });
    }
    out.tableEnd(start);
  } else if constexpr (std::is_same_v<T, std::string>) {
    out.value(o);
  } else if constexpr (std::is_convertible_v<T, std::string_view>) {
    out.value(std::string_view(o));
  } else if constexpr (is_optional_v<T>) {
    if (o.has_value()) {
      serialize(o.value(), out);
      out.value(Optional::HasValue);
    } else {
      out.value(Optional::NullOpt);
    }
  } else if constexpr (is_unique_ptr_v<T> || is_shared_ptr_v<T>) {
    if (o) {
      serialize(*o, out);
      out.value(Optional::HasValue);
    } else {
      out.value(Optional::NullOpt);
    }
  } else if constexpr (is_variant_v<T> && isBinaryOut) {
    static_assert(std::variant_size_v<T> <= std::numeric_limits<uint8_t>::max());
    std::visit(
        [&out](auto &&v) {
          serialize(v, out);
          serialize(type_name_v<std::decay_t<decltype(v)>>, out);
        },
        o);
  } else if constexpr (is_variant_v<T>) {
    static_assert(std::variant_size_v<T> <= std::numeric_limits<uint8_t>::max());
    auto start = out.tableBegin(true);
    out.key("type");
    std::visit(
        [&out](auto &&v) {
          serialize(type_name_v<std::decay_t<decltype(v)>>, out);
          out.key("value");
          serialize(v, out);
        },
        o);
    out.tableEnd(start);
  } else if constexpr (is_generic_pair_v<T> && isBinaryOut) {
    serialize(o.second, out);
    serialize(o.first, out);
  } else if constexpr (Container<T> && isBinaryOut) {
    out.arrayBegin();
    if constexpr (requires { o.rbegin(); }) {
      for (auto it = o.rbegin(); it != o.rend(); ++it) {
        serialize(*it, out);
      }
    } else {
      for (auto &item : o) {
        serialize(item, out);
      }
    }
    out.arrayEnd(o.size());
  } else if constexpr (is_vector_v<T> && std::is_arithmetic_v<T> && isBinaryOut) {
    out.value(o);
  } else if constexpr (is_vector_v<T> || is_set_v<T>) {
    out.arrayBegin();
    for (const auto &item : o) {
      serialize(item, out);
    }
    out.arrayEnd(o.size());
  } else if constexpr (is_map_v<T>) {
    auto start = out.tableBegin(true);
    for (const auto &pair : o) {
      if constexpr (requires { out.key(pair.first); }) {
        out.key(pair.first);
      } else if constexpr (requires { pair.first.toUnderType(); }) {
        out.key(fmt::format("{}", pair.first.toUnderType()));
      } else {
        out.key(fmt::format("{}", pair.first));
      }
      serialize(pair.second, out);
    }
    out.tableEnd(start);
  } else if constexpr (isBinaryOut && SerdeCopyable<T>) {
    out.value(o);
  } else if constexpr (std::is_same_v<T, bool>) {
    out.value(o);
  } else if constexpr (std::is_integral_v<T>) {
    out.value(int64_t(o));
  } else if constexpr (std::is_floating_point_v<T>) {
    out.value(double(o));
  } else if constexpr (std::is_enum_v<T>) {
    out.value(magic_enum::enum_name(o));
  } else {
    return notSupportToSerialize(o);
  }
}

struct JsonObject;
struct TomlObject;

template <class T>
concept IsJsonOrToml = std::is_same_v<T, JsonObject> || std::is_same_v<T, TomlObject>;
template <class T>
concept IsBinaryOut = std::is_same_v<T, std::string> || is_specialization_of_v<T, DownwardBytes>;

template <IsJsonOrToml T>
class Out<T> {
 public:
  Out();
  ~Out();
  int tableBegin(bool);
  void tableEnd(auto) { end(); }
  void arrayBegin();
  void arrayEnd(uint32_t) { end(); }
  void end();

  void key(std::string_view key) { add(key); }
  void value(bool value) { add(value); }
  void value(int64_t value) { add(value); }
  void value(double value) { add(value); }
  void value(std::string &&str) { add(std::move(str)); }
  void value(std::string_view str) { add(str); }
  void value(Optional);
  void value(VariantIndex) {}

  std::string toString(bool sortKeys = false, bool prettyFormatting = false);

 protected:
  void add(auto &&v);
  bool inTable();
  bool inArray();

 private:
  std::vector<T> root_;
};

template <IsBinaryOut T>
class Out<T> {
 public:
  using is_binary_out = void;

  uint32_t tableBegin(bool) { return out_.size(); }
  void tableEnd(uint32_t start) { serde::serialize(Varint32(out_.size() - start), *this); }
  void arrayBegin() {}
  void arrayEnd(uint32_t size) { serde::serialize(Varint32(size), *this); }

  inline void key(std::string_view) {}  // ignore key.
  void value(auto &&value) requires(std::is_trivially_copyable_v<std::decay_t<decltype(value)>>) {
    append(&value, sizeof(value));
  }
  void value(std::string_view str) {
    append(str.data(), str.size());
    serde::serialize(Varint32(str.size()), *this);
  }
  template <class V>
  requires(std::is_arithmetic_v<V>) void value(const std::vector<V> &vec) {
    append(vec.data(), vec.size() * sizeof(V));
    serde::serialize(Varint32(vec.size()), *this);
  }

  auto &bytes() { return out_; }

  inline void append(auto *data, size_t size) { out_.append(reinterpret_cast<const char *>(data), size); }

 private:
  T out_;
};

struct UnknownVariantType {
  SERDE_STRUCT_FIELD(type, String{});
  // TODO: add serializedBytes
};

template <typename... Ts>
using AutoFallbackVariant = std::variant<UnknownVariantType, Ts...>;

template <typename T>
constexpr bool is_auto_fallback_variant_v = false;

template <typename... Ts>
constexpr bool is_auto_fallback_variant_v<AutoFallbackVariant<Ts...>> = true;

inline Result<Void> deserialize(auto &o, auto &&in) requires is_specialization<std::decay_t<decltype(in)>, In> {
  using T = std::decay_t<decltype(o)>;
  using I = std::decay_t<decltype(in)>;
  constexpr bool isBinaryIn = requires { typename I::is_binary_in; };
  if constexpr (!isBinaryIn && requires { function_first_parameter_t<&T::serdeFromReadable>{}; }) {
    // 0. custom serde impl.
    function_first_parameter_t<&T::serdeFromReadable> from{};
    RETURN_AND_LOG_ON_ERROR(deserialize(from, in));
    auto result = T::serdeFromReadable(from);
    RETURN_AND_LOG_ON_ERROR(result);
    o = std::move(*result);
    return Void{};
  } else if constexpr (!isBinaryIn && requires { function_first_parameter_t<&SerdeMethod<T>::serdeFromReadable>{}; }) {
    function_first_parameter_t<&SerdeMethod<T>::serdeFromReadable> from{};
    RETURN_AND_LOG_ON_ERROR(deserialize(from, in));
    auto result = SerdeMethod<T>::serdeFromReadable(from);
    RETURN_AND_LOG_ON_ERROR(result);
    o = std::move(*result);
    return Void{};
  } else if constexpr (requires { function_first_parameter_t<&SerdeMethod<T>::serdeFrom>{}; }) {
    // 0. custom serde impl.
    function_first_parameter_t<&SerdeMethod<T>::serdeFrom> from{};
    RETURN_AND_LOG_ON_ERROR(deserialize(from, in));
    auto result = SerdeMethod<T>::serdeFrom(from);
    RETURN_AND_LOG_ON_ERROR(result);
    o = std::move(*result);
    return Void{};
  } else if constexpr (!isBinaryIn && requires { SerdeMethod<T>::deserializeReadable(o, in); }) {
    return SerdeMethod<T>::deserializeReadable(o, in);
  } else if constexpr (requires { SerdeMethod<T>::deserialize(o, in); }) {
    return SerdeMethod<T>::deserialize(o, in);
  } else if constexpr (SerdeType<T>) {
    auto table = in.parseTable();
    RETURN_AND_LOG_ON_ERROR(table);
    if constexpr (isBinaryIn) {
      return refl::Helper::iterate<T>(
          [&](auto type) -> Result<Void> {
            if (LIKELY(*table)) {
              return deserialize(o.*type.getter, *table);
            }
            // Missing fields at the end are acceptable.
            return Void{};
          },
          [&]() -> Result<Void> {
            table = in.parseTable();
            RETURN_AND_LOG_ON_ERROR(table);
            return Void{};
          });
    } else {
      return refl::Helper::iterate<T>([&](auto type) -> Result<Void> {
        auto value = table->parseKey(type.name);
        if (LIKELY(bool(value))) {
          return deserialize(o.*type.getter, *value);
        } else {
          using ItemType = std::decay_t<decltype(o.*type.getter)>;
          if constexpr (is_optional_v<ItemType>) {
            o.*type.getter = std::nullopt;
          } else if constexpr (is_unique_ptr_v<ItemType> || is_shared_ptr_v<ItemType>) {
            o.*type.getter = nullptr;
          }
        }
        return Void{};
      });
    }
  } else if constexpr (std::is_same_v<std::string, T> || std::is_same_v<std::string_view, T>) {
    auto result = in.parseString();
    RETURN_AND_LOG_ON_ERROR(result);
    o = *result;
    return Void{};
  } else if constexpr (is_optional_v<T>) {
    Optional optional;
    RETURN_AND_LOG_ON_ERROR(in.parseOptional(optional));
    if (optional == Optional::HasValue) {
      std::remove_cv_t<typename T::value_type> value;
      RETURN_AND_LOG_ON_ERROR(deserialize(value, in));
      o = std::move(value);
    } else {
      o = std::nullopt;
    }
    return Void{};
  } else if constexpr (is_unique_ptr_v<T> || is_shared_ptr_v<T>) {
    Optional optional;
    RETURN_AND_LOG_ON_ERROR(in.parseOptional(optional));
    if (optional == Optional::HasValue) {
      if constexpr (is_unique_ptr_v<T>) {
        o = std::make_unique<typename T::element_type>();
      } else {
        o = std::make_shared<typename T::element_type>();
      }
      RETURN_AND_LOG_ON_ERROR(deserialize(*o, in));
    } else {
      o = nullptr;
    }
    return Void{};
  } else if constexpr (is_variant_v<T>) {
    auto variant = in.parseVariant();
    RETURN_AND_LOG_ON_ERROR(variant);
    return callByIdx<type_list_t<T>>(
        [&](auto type) -> Result<Void> {
          if constexpr (std::is_same_v<decltype(type), std::nullptr_t>) {
            if constexpr (is_auto_fallback_variant_v<T>) {
              UnknownVariantType uvt;
              uvt.type = variant->first;
              o = std::move(uvt);
              return Void{};
            } else {
              return makeError(StatusCode::kSerdeVariantIndexExceeded);
            }
          } else {
            RETURN_AND_LOG_ON_ERROR(deserialize(type, variant->second));
            o = std::move(type);
            return Void{};
          }
        },
        variantTypeNameToIndex<T>(variant->first));
    return Void{};
  } else if constexpr (is_generic_pair_v<T> && isBinaryIn) {
    RETURN_AND_LOG_ON_ERROR(deserialize(o.first, in));
    RETURN_AND_LOG_ON_ERROR(deserialize(o.second, in));
    return Void{};
  } else if constexpr (Container<T> && isBinaryIn) {
    Varint64 size = 0;
    RETURN_AND_LOG_ON_ERROR(deserialize(size, in));
    o.clear();
    if constexpr (requires { o.reserve(size); }) {
      o.reserve(size);
    }
    auto inserter = std::inserter(o, o.end());
    for (uint32_t i = 0; i < size; ++i) {
      if constexpr (is_generic_pair_v<typename T::value_type>) {
        std::remove_cv_t<typename T::value_type::first_type> first;
        RETURN_AND_LOG_ON_ERROR(deserialize(first, in));
        std::remove_cv_t<typename T::value_type::second_type> second;
        RETURN_AND_LOG_ON_ERROR(deserialize(second, in));
        inserter++ = typename T::value_type{std::move(first), std::move(second)};
      } else {
        auto value = DefaultConstructor<std::remove_cv_t<typename T::value_type>>::construct();
        RETURN_AND_LOG_ON_ERROR(deserialize(value, in));
        inserter++ = std::move(value);
      }
    }
    return Void{};
  } else if constexpr (is_vector_v<T> || is_set_v<T> || is_map_v<T>) {
    auto containerResult = in.parseContainer();
    RETURN_AND_LOG_ON_ERROR(containerResult);
    auto size = containerResult->first;
    auto it = std::move(containerResult->second);
    o.clear();
    if constexpr (requires { o.reserve(size); }) {
      o.reserve(size);
    }
    auto inserter = std::inserter(o, o.end());
    for (uint32_t i = 0; i < size; ++i) {
      if constexpr (is_map_v<T>) {
        auto keyResult = in.fetchKey(it);
        RETURN_AND_LOG_ON_ERROR(keyResult);
        std::string_view key = *keyResult;
        using KeyType = std::remove_cv_t<typename T::value_type::first_type>;
        KeyType first;
        if constexpr (requires { first = KeyType{key}; }) {
          first = KeyType{key};
        } else if constexpr (requires { scn::scan(key, "{}", first); }) {
          auto result = scn::scan(key, "{}", first);
          if (!result) {
            return makeError(StatusCode::kInvalidArg);
          }
        } else if constexpr (requires { scn::scan(key, "{}", first.toUnderType()); }) {
          auto result = scn::scan(key, "{}", first.toUnderType());
          if (!result) {
            return makeError(StatusCode::kInvalidArg);
          }
        } else {
          return makeError(StatusCode::kInvalidArg);
        }
        std::remove_cv_t<typename T::value_type::second_type> second;
        RETURN_AND_LOG_ON_ERROR(deserialize(second, in.fetchValue(it)));
        inserter++ = typename T::value_type{std::move(first), std::move(second)};
        in.next(it);
      } else {
        std::remove_cv_t<typename T::value_type> value;
        RETURN_AND_LOG_ON_ERROR(deserialize(value, in.fetchAndNext(it)));
        inserter++ = std::move(value);
      }
    }
    return Void{};
  } else if constexpr (isBinaryIn && SerdeCopyable<T>) {
    return in.parseCopyable(o);
  } else if constexpr (std::is_same_v<T, bool>) {
    auto result = in.parseBoolean();
    RETURN_AND_LOG_ON_ERROR(result);
    o = *result;
    return Void{};
  } else if constexpr (std::is_integral_v<T>) {
    auto result = in.parseInteger();
    RETURN_AND_LOG_ON_ERROR(result);
    o = *result;
    return Void{};
  } else if constexpr (std::is_floating_point_v<T>) {
    auto result = in.parseFloat();
    RETURN_AND_LOG_ON_ERROR(result);
    o = *result;
    return Void{};
  } else if constexpr (std::is_enum_v<T>) {
    std::string_view str;
    RETURN_AND_LOG_ON_ERROR(deserialize(str, in));
    auto result = magic_enum::enum_cast<T>(str);
    if (result.has_value()) {
      o = *result;
      return Void{};
    }
    return makeError(StatusCode::kSerdeUnknownEnumValue,
                     fmt::format("unknown enum value {} for type {}", str, nameof::nameof_full_type<T>()));
  } else {
    return notSupportToDeserialize(o, in);
  }
}

template <>
class In<std::string_view> {
 public:
  using is_binary_in = void;

  In(std::string_view str)
      : str_(str) {}

  operator bool() const { return !str_.empty(); }

  Result<In> parseTable() {
    Varint64 length;
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(length, *this));
    if (UNLIKELY(length > str_.length())) {
      return makeError(StatusCode::kSerdeInsufficientLength,
                       fmt::format("string short {} > {}", length, str_.length()));
    }
    auto table = In{str_.substr(0, length)};
    str_.remove_prefix(length);
    return table;
  }

  Result<std::string_view> parseString() {
    Varint64 length;
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(length, *this));
    if (UNLIKELY(length > str_.length())) {
      return makeError(StatusCode::kSerdeInsufficientLength,
                       fmt::format("string short {} > {}", length, str_.length()));
    }
    auto value = str_.substr(0, length);
    str_.remove_prefix(length);
    return value;
  }

  Result<std::pair<std::string_view, In &>> parseVariant() {
    std::string_view typeName;
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(typeName, *this));
    return std::pair<std::string_view, In &>(typeName, *this);
  }

  Result<Void> parseOptional(Optional &optional) { return serde::deserialize(optional, *this); }

  Result<Void> parseCopyable(auto &o) {
    if (UNLIKELY(sizeof(o) > str_.length())) {
      return makeError(StatusCode::kSerdeInsufficientLength,
                       fmt::format("trivially copyable {} > {}", sizeof(o), str_.length()));
    }
    std::memcpy(&o, str_.data(), sizeof(o));
    str_.remove_prefix(sizeof(o));
    return Void{};
  }

  auto &str() { return str_; }

 private:
  std::string_view str_;
};

template <>
class In<JsonObject> {
 public:
  In(const JsonObject &obj)
      : obj_(obj) {}
  template <class T>
  In(const T &obj);
  static Result<Void> parse(std::string_view str, std::function<Result<Void>(const JsonObject &)> func);

  Result<In> parseKey(std::string_view key) const;
  Result<In> parseTable() const;

  Result<bool> parseBoolean() const;
  Result<int64_t> parseInteger() const;
  Result<double> parseFloat() const;

  Result<std::string_view> parseString() const;
  Result<std::pair<std::string_view, In>> parseVariant() const;

  Result<Void> parseOptional(Optional &optional) const;

  Result<std::pair<uint32_t, std::unique_ptr<void, void (*)(void *)>>> parseContainer() const;

  In fetchAndNext(std::unique_ptr<void, void (*)(void *)> &it) const;
  Result<std::string_view> fetchKey(std::unique_ptr<void, void (*)(void *)> &it) const;
  In fetchValue(std::unique_ptr<void, void (*)(void *)> &it) const;
  void next(std::unique_ptr<void, void (*)(void *)> &it) const;

 private:
  const JsonObject &obj_;
};

template <>
class In<TomlObject> {
 public:
  In(const TomlObject &obj)
      : obj_(obj) {}
  template <class T>
  In(const T &obj);
  static Result<Void> parse(std::string_view str, std::function<Result<Void>(const TomlObject &)> func);
  static Result<Void> parseFile(const Path &path, std::function<Result<Void>(const TomlObject &)> func);

  Result<In> parseKey(std::string_view key) const;
  Result<In> parseTable() const;

  Result<bool> parseBoolean() const;
  Result<int64_t> parseInteger() const;
  Result<double> parseFloat() const;

  Result<std::string_view> parseString() const;
  Result<std::pair<std::string_view, In>> parseVariant() const;

  Result<Void> parseOptional(Optional &optional) const;

  Result<std::pair<uint32_t, std::unique_ptr<void, void (*)(void *)>>> parseContainer() const;

  In fetchAndNext(std::unique_ptr<void, void (*)(void *)> &it) const;
  Result<std::string_view> fetchKey(std::unique_ptr<void, void (*)(void *)> &it) const;
  In fetchValue(std::unique_ptr<void, void (*)(void *)> &it) const;
  void next(std::unique_ptr<void, void (*)(void *)> &it) const;

 private:
  const TomlObject &obj_;
};

inline std::string serialize(const auto &o) {
  Out<DownwardBytes<net::Allocator<>>> out;
  serialize(o, out);
  return out.bytes().toString();
}

inline DownwardBytes<net::Allocator<>> serializeBytes(const auto &o) {
  Out<DownwardBytes<net::Allocator<>>> out;
  serialize(o, out);
  return std::move(out.bytes());
}

inline void serializeToUserBuffer(const auto &o, uint8_t *data, uint32_t capacity) {
  Out<DownwardBytes<UserBufferAllocator>> out;
  out.bytes().setBuffer(data, capacity);
  serialize(o, out);
}

inline auto serializeLength(const auto &o) {
  Out<DownwardBytes<void>> out;
  serialize(o, out);
  return out.bytes().size();
}

inline Result<Void> deserialize(auto &o, std::string_view str) {
  serde::In<std::string_view> in(str);
  return deserialize(o, in);
}

inline std::string toTomlString(const auto &o) {
  Out<TomlObject> out;
  serialize(o, out);
  return out.toString();
}

inline std::string toJsonString(const auto &o, bool sortKeys = false, bool prettyFormatting = false) {
  Out<JsonObject> out;
  serialize(o, out);
  return out.toString(sortKeys, prettyFormatting);
}

inline Result<Void> fromJsonString(auto &o, std::string_view str) {
  return In<JsonObject>::parse(str, [&](const JsonObject &obj) { return deserialize(o, In<JsonObject>(obj)); });
}

inline Result<Void> fromTomlString(auto &o, std::string_view str) {
  return In<TomlObject>::parse(str, [&](const TomlObject &obj) { return deserialize(o, In<TomlObject>(obj)); });
}

inline Result<Void> fromTomlFile(auto &o, const Path &path) {
  return In<TomlObject>::parseFile(path, [&](const TomlObject &obj) { return deserialize(o, In<TomlObject>(obj)); });
}

template <typename T>
concept SerializableToBytes = requires(const T &o) {
  serialize(o);
};

template <typename T>
concept SerializableToToml = requires(const T &o) {
  toTomlString(o);
};

template <typename T>
concept SerializableToJson = requires(const T &o) {
  toJsonString(o);
};

template <typename T>
concept Serializable = requires(const T &o) {
  serialize(o);
  toJsonString(o);
};

}  // namespace hf3fs::serde

template <>
struct ::hf3fs::serde::SerdeMethod<::hf3fs::Void> {
  static constexpr auto serialize(Void, auto &&) {}                 // DO NOTHING.
  static Result<Void> deserialize(Void, auto &) { return Void{}; }  // DO NOTHING.
  static constexpr std::string_view serdeToReadable(Void) { return "Void"; };
};

template <>
struct ::hf3fs::serde::SerdeMethod<::hf3fs::Status> {
  static constexpr auto serialize(const Status &status, auto &out) {
    std::optional<std::string_view> msg = std::nullopt;
    if (!status.message().empty()) {
      msg = status.message();
    }
    serde::serialize(msg, out);
    serde::serialize(status.code(), out);
  }

  static Result<Void> deserialize(Status &status, auto &&in) {
    status_code_t code;
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(code, in));
    std::optional<std::string_view> msg;
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(msg, in));
    if (msg.has_value()) {
      status = Status(code, msg.value());
    } else {
      status = Status(code);
    }
    return Void{};
  }

  struct InnerStatus {
    SERDE_STRUCT_FIELD(code, uint16_t{});
    SERDE_STRUCT_FIELD(msg, std::string{});
  };

  static auto serdeToReadable(const Status &status) {
    return InnerStatus{status.code(), std::string(status.message())};
  }

  static Result<Status> serdeFromReadable(const InnerStatus &inner) {
    if (inner.msg.empty()) {
      return folly::makeExpected<Status>(Status{inner.code});
    } else {
      return folly::makeExpected<Status>(Status{inner.code, inner.msg});
    }
  }
};

template <class T>
struct ::hf3fs::serde::SerdeMethod<::hf3fs::Result<T>> {
  static auto serialize(const ::hf3fs::Result<T> &result, auto &out) {
    bool hasValue = result.hasValue();
    if (hasValue) {
      serde::serialize(result.value(), out);
    } else {
      serde::serialize(result.error(), out);
    }
    out.value(hasValue);
  }

  static auto serializeReadable(const ::hf3fs::Result<T> &result, auto &out) {
    auto start = out.tableBegin(false);
    {
      if (result.hasValue()) {
        out.key("value");
        serde::serialize(result.value(), out);
      } else {
        out.key("error");
        serde::serialize(result.error(), out);
      }
    }
    out.tableEnd(start);
  }

  static Result<Void> deserialize(::hf3fs::Result<T> &result, auto &&in) {
    bool hasValue = false;
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(hasValue, in));
    if (hasValue) {
      T value;
      RETURN_AND_LOG_ON_ERROR(serde::deserialize(value, in));
      result = std::move(value);
    } else {
      Status status = Status::OK;
      RETURN_AND_LOG_ON_ERROR(serde::deserialize(status, in));
      result = makeError(std::move(status));
    }
    return Void{};
  }
};

template <>
struct hf3fs::serde::SerdeMethod<hf3fs::Path> {
  static auto serdeTo(const hf3fs::Path &p) { return p.string(); }
  static Result<hf3fs::Path> serdeFrom(std::string_view str) { return hf3fs::Path{std::string{str}}; }
};

FMT_BEGIN_NAMESPACE

template <::hf3fs::serde::SerdeType T>
requires(::hf3fs::serde::SerializableToJson<T>) struct formatter<T> : formatter<std::string_view> {
  detail::dynamic_format_specs<char> specs_;
  /* Copy from https://fmt.dev/latest/api.html#formatting-user-defined-types */
  template <typename ParseContext>
  constexpr auto parse(ParseContext &ctx) -> decltype(ctx.begin()) {
    auto end = detail::parse_format_specs(ctx.begin(), ctx.end(), specs_, ctx, detail::type::char_type);
    detail::check_char_specs(specs_);
    return end;
  }

  template <typename FormatContext>
  auto format(const T &t, FormatContext &ctx) const {
    bool debug = specs_.type == presentation_type::debug;
    return formatter<std::string_view>::format(
        hf3fs::serde::toJsonString(t, debug /*sortKeys*/, debug /*prettyFormatting*/),
        ctx);
  }
};

FMT_END_NAMESPACE
