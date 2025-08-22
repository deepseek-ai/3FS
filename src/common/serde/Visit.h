#pragma once

#include <type_traits>
#include <utility>
#include <variant>

#include "common/serde/Serde.h"
#include "common/utils/Nameof.hpp"

namespace hf3fs::serde {

template <class T>
class VisitOut {
 public:
  auto tableBegin();               // begin a table.
  void tableEnd();                 // end a table.
  void arrayBegin();               // begin an array.
  void arrayEnd();                 // end an array.
  void variantBegin();             // begin a variant.
  void variantEnd();               // end a variant.
  void key(std::string_view key);  // prepare the key in a key-value pair.
  void value(auto &&value);        // prepare the value in a key-value pair or an array.
};

template <size_t I = 0>
inline void visitVariant(auto &&t, auto &&func) {
  using T = std::decay_t<decltype(t)>;
  using S = std::variant_alternative_t<I, T>;
  if (t.index() == I) {
    func(nameof::nameof_type<S>(), std::get<I>(t));
  } else {
    func(nameof::nameof_type<S>(), std::variant_alternative_t<I, T>{});
  }
  if constexpr (I + 1 < std::variant_size_v<T>) {
    visitVariant<I + 1>(t, func);
  }
}

template <class O>
inline void visit(auto &&t, VisitOut<O> &out) {
  using T = std::decay_t<decltype(t)>;
  if constexpr (SerdeType<T>) {
    out.tableBegin();
    refl::Helper::iterate<T>([&](auto type) {
      out.key(type.name);
      visit(t.*type.getter, out);
    });
    out.tableEnd();
  } else if constexpr (std::is_same_v<T, std::string> || std::is_convertible_v<T, std::string_view>) {
    out.value(std::string_view(t));
  } else if constexpr (is_variant_v<T>) {
    out.variantBegin();
    visitVariant(t, [&](std::string_view name, auto &&t) {
      out.key(name);
      visit(t, out);
    });
    out.variantEnd();
  } else if constexpr (is_vector_v<T> || is_set_v<T>) {
    out.arrayBegin();
    for (auto item : t) {
      visit(item, out);
    }
    out.arrayEnd();
  } else {
    out.value(t);
  }
}

}  // namespace hf3fs::serde
