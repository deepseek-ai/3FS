#pragma once

#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "Common.h"
#include "common/serde/Serde.h"
#include "common/utils/Nameof.hpp"
#include "common/utils/StrongType.h"
#include "common/utils/TypeTraits.h"

namespace hf3fs::analytics {

class ObjectVisitor {
 public:
  // default
  template <typename T>
  void visit(std::string_view, T &) = delete;

  template <typename T>
  requires std::is_arithmetic_v<T>
  void visit(std::string_view, T &) = delete;

  template <typename T>
  requires std::is_enum_v<T>
  void visit(std::string_view, T &) = delete;

  template <StrongTyped T>
  void visit(std::string_view, T &) = delete;

  template <serde::ConvertibleToString T>
  void visit(std::string_view, T &) = delete;

  template <serde::SerdeTypeWithoutSpecializedSerdeMethod T>
  void visit(std::string_view, T &) = delete;

  template <typename T>
  requires is_variant_v<T>
  void visit(std::string_view, T &) = delete;

  template <typename T>
  requires is_vector_v<T> || is_set_v<T>
  void visit(std::string_view, T &) = delete;

  template <typename T>
  requires is_optional_v<T>
  void visit(std::string_view, T &) = delete;
};

template <size_t I = 0>
inline void visitVariant(auto &&t, auto &&func) {
  using T = std::decay_t<decltype(t)>;
  using S = std::variant_alternative_t<I, T>;
  if (t.index() == I) {
    func(nameof::nameof_short_type<S>(), std::get<I>(t));
  } else {
    func(nameof::nameof_short_type<S>(), std::variant_alternative_t<I, T>{});
  }
  if constexpr (I + 1 < std::variant_size_v<T>) {
    visitVariant<I + 1>(t, func);
  }
}

template <typename Derived>
class BaseObjectVisitor : public ObjectVisitor {
 public:
  template <typename T>
  void visit(std::string_view k, T &) = delete;

  template <typename T>
  requires std::is_arithmetic_v<T>
  void visit(std::string_view k, T &v) {
    XLOGF(DBG3, "arithmetic visit({})", k);
    static_cast<Derived *>(this)->template visit<T>(k, v);
  }

  template <typename T>
  requires std::is_enum_v<T>
  void visit(std::string_view k, T &) = delete;

  template <serde::ConvertibleToString T>
  void visit(std::string_view k, T &) = delete;

  template <StrongTyped T>
  void visit(std::string_view k, T &v) {
    XLOGF(DBG3, "strongtyped visit({})", k);
    static_cast<Derived *>(this)->template visit<typename T::UnderlyingType>(k, v);
  }

  template <serde::SerdeTypeWithoutSpecializedSerdeMethod T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "serdetype visit({})", k);
    refl::Helper::iterate<T>(
        [&](auto type) { static_cast<Derived *>(this)->template visit(type.name, val.*type.getter); });
  }

  template <typename T>
  requires is_variant_v<T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "variant visit({})", k);
    visitVariant(val, [&](std::string_view typeName, auto &&v) {
      std::string altTypeName = std::string{k} + std::string{typeName};
      static_cast<Derived *>(this)->template visit(altTypeName, v);
    });
  }

  template <typename T>
  requires is_vector_v<T> || is_set_v<T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "container visit({})", k);
    for (auto item : val) {
      static_cast<Derived *>(this)->template visit(k, item);
    }
  }

  template <typename T>
  requires is_optional_v<T>
  void visit(std::string_view k, T &val) {
    XLOGF(DBG3, "optional visit({})", k);
    using ValueType = typename T::value_type;
    if (val.has_value()) {
      static_cast<Derived *>(this)->template visit<ValueType>(k, *val);
    }
  }
};

}  // namespace hf3fs::analytics
