#pragma once

#include "common/serde/Serde.h"
#include "common/utils/StrongType.h"

namespace hf3fs::serde {

template <typename T>
using SerdeToReadableMemberMethodReturnType = std::invoke_result_t<decltype(&T::serdeToReadable), T>;

template <typename T>
using SerdeToMemberMethodReturnType = std::invoke_result_t<decltype(&T::serdeTo), T>;

template <typename T>
using SerdeToReadableReturnType = std::invoke_result_t<decltype(serde::SerdeMethod<T>::serdeToReadable), const T &>;

template <typename T>
using SerdeToReturnType = std::invoke_result_t<decltype(serde::SerdeMethod<T>::serdeTo), const T &>;

template <typename T>
concept ConvertibleToString = std::is_convertible_v<T, std::string>;

template <typename T>
concept WithReadableSerdeMemberMethod =
    !StrongTyped<T> && !ConvertibleToString<T> && requires(const T &t, SerdeToReadableMemberMethodReturnType<T> s) {
  { t.serdeToReadable() } -> std::convertible_to<SerdeToReadableMemberMethodReturnType<T>>;
  { T::serdeFromReadable(s) } -> std::convertible_to<Result<T>>;
};

template <typename T>
concept WithReadableSerdeMethod = !StrongTyped<T> && !ConvertibleToString<T> && !WithReadableSerdeMemberMethod<T> &&
                                  requires(const T &t, SerdeToReadableReturnType<T> s) {
  { serde::SerdeMethod<T>::serdeToReadable(t) } -> std::convertible_to<SerdeToReadableReturnType<T>>;
  { serde::SerdeMethod<T>::serdeFromReadable(s) } -> std::convertible_to<Result<T>>;
};

template <typename T>
concept WithSerdeMemberMethod =
    !StrongTyped<T> && !ConvertibleToString<T> && !WithReadableSerdeMemberMethod<T> && !WithReadableSerdeMethod<T> &&
    requires(const T &t, SerdeToMemberMethodReturnType<T> s) {
  { t.serdeTo() } -> std::convertible_to<SerdeToMemberMethodReturnType<T>>;
  { T::serdeFrom(s) } -> std::convertible_to<Result<T>>;
};

template <typename T>
concept WithSerdeMethod =
    !StrongTyped<T> && !ConvertibleToString<T> && !WithReadableSerdeMemberMethod<T> && !WithReadableSerdeMethod<T> &&
    !WithSerdeMemberMethod<T> && requires(const T &t, SerdeToReturnType<T> s) {
  { serde::SerdeMethod<T>::serdeTo(t) } -> std::convertible_to<SerdeToReturnType<T>>;
  { serde::SerdeMethod<T>::serdeFrom(s) } -> std::convertible_to<Result<T>>;
};

template <typename T>
concept SerdeTypeWithoutSpecializedSerdeMethod =
    serde::SerdeType<T> && !WithReadableSerdeMemberMethod<T> && !WithReadableSerdeMethod<T> &&
    !WithSerdeMemberMethod<T> && !WithSerdeMethod<T>;

}  // namespace hf3fs::serde

namespace hf3fs::analytics {

const std::string kVariantValueIndexColumnSuffix = "ValIdx";
const std::string kResultErrorTypeColumnSuffix = "Error";

}  // namespace hf3fs::analytics
