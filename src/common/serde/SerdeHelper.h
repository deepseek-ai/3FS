#pragma once

#include "Serde.h"
#include "common/kv/ITransaction.h"

namespace hf3fs::serde {
template <typename T>
struct SerdeHelper {
  using Self = T;

  template <typename... Args>
  static Self create(Args &&...args) {
    return Self{/* base */ {}, std::forward<Args>(args)...};
  }

  CoTryTask<void> store(kv::IReadWriteTransaction &txn, std::string_view key) const {
    auto value = serialize(*static_cast<const T *>(this));
    co_return co_await txn.set(key, value);
  }

  static CoTryTask<std::optional<T>> load(kv::IReadOnlyTransaction &txn, std::string_view key) {
    co_return unpackFrom(co_await txn.get(key));
  }

  static CoTryTask<std::optional<T>> snapshotLoad(kv::IReadOnlyTransaction &txn, std::string_view key) {
    co_return unpackFrom(co_await txn.snapshotGet(key));
  }

  static Result<T> unpackFrom(std::string_view str) {
    auto o = defaultValue();
    RETURN_ON_ERROR(deserialize(o, str));
    return o;
  }

  static Result<T> unpackFrom(const String &str) { return unpackFrom(std::string_view(str)); }

  static Result<std::optional<T>> unpackFrom(const Result<std::optional<String>> &getResult) {
    RETURN_ON_ERROR(getResult);

    if (!getResult->has_value()) {
      return std::nullopt;
    } else {
      auto o = defaultValue();
      RETURN_ON_ERROR(deserialize(o, getResult->value()));
      return o;
    }
  }

  static T defaultValue() {
    if constexpr (requires { T::defaultValueForLoad(); }) {
      return T::defaultValueForLoad();
    } else {
      return T{};
    }
  }
};
}  // namespace hf3fs::serde

#define DEFINE_SERDE_HELPER_STRUCT(name) struct name : public ::hf3fs::serde::SerdeHelper<name>
