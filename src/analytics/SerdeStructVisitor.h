#pragma once

#include <folly/logging/xlog.h>
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

class StructVisitor {
 public:
  // default
  template <typename T>
  void visit(std::string_view) = delete;

  template <typename T>
  requires std::is_arithmetic_v<T>
  void visit(std::string_view) = delete;

  template <typename T>
  requires std::is_enum_v<T>
  void visit(std::string_view) = delete;

  template <StrongTyped T>
  void visit(std::string_view) = delete;

  template <serde::SerdeTypeWithoutSpecializedSerdeMethod T>
  void visit(std::string_view) = delete;

  template <typename T>
  requires is_variant_v<T>
  void visit(std::string_view) = delete;

  template <typename T>
  requires is_vector_v<T> || is_set_v<T>
  void visit(std::string_view) = delete;

  template <typename T>
  requires is_optional_v<T>
  void visit(std::string_view) = delete;
};

template <class T, size_t I = 0>
inline void visitVariant(auto &&func) {
  using S = std::variant_alternative_t<I, T>;
  func(nameof::nameof_short_type<S>(), std::type_identity<S>{});
  if constexpr (I + 1 < std::variant_size_v<T>) {
    visitVariant<T, I + 1>(func);
  }
}

template <typename Derived>
class BaseStructVisitor : public StructVisitor {
 public:
  // default
  template <typename T>
  void visit(std::string_view k) = delete;

  template <typename T>
  requires std::is_arithmetic_v<T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "arithmetic visit({})", k);
    static_cast<Derived *>(this)->template visit<T>(k);
  }

  template <typename T>
  requires std::is_enum_v<T>
  void visit(std::string_view k) = delete;

  template <serde::ConvertibleToString T>
  void visit(std::string_view k) = delete;

  template <StrongTyped T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "strongtyped visit({})", k);
    static_cast<Derived *>(this)->template visit<typename T::UnderlyingType>(k);
  }

  template <serde::SerdeTypeWithoutSpecializedSerdeMethod T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "serdetype visit({})", k);
    refl::Helper::iterate<T>([&](auto field) {
      using FieldType = std::decay_t<decltype(std::declval<T>().*field.getter)>;
      static_cast<Derived *>(this)->template visit<FieldType>(field.name);
    });
  }

  template <typename T>
  requires is_variant_v<T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "variant visit({})", k);
    visitVariant<T>([&](std::string_view typeName, auto &&v) {
      using AlternativeType = typename std::decay_t<decltype(v)>::type;
      std::string altTypeName = std::string{k} + std::string{typeName};
      static_cast<Derived *>(this)->template visit<AlternativeType>(altTypeName);
    });
  }

  template <typename T>
  requires is_vector_v<T> || is_set_v<T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "container visit({})", k);
    using ElemValueType = typename T::value_type;
    static_cast<Derived *>(this)->template visit<ElemValueType>(k);
  }

  template <typename T>
  requires is_optional_v<T>
  void visit(std::string_view k) {
    XLOGF(DBG3, "optional visit({})", k);
    using ValueType = typename T::value_type;
    static_cast<Derived *>(this)->template visit<ValueType>(k);
  }
};

}  // namespace hf3fs::analytics
