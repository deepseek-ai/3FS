#pragma once

#include "common/serde/Serde.h"

namespace hf3fs::serde {

template <class T>
struct BigEndian {
  BigEndian() = default;
  BigEndian(T v)
      : value(v) {}

  T value;
  operator T() const { return value; }
};

}  // namespace hf3fs::serde

template <class T>
struct hf3fs::serde::SerdeMethod<hf3fs::serde::BigEndian<T>> {
  static auto serdeTo(const BigEndian<T> &o) { return folly::Endian::swap(o.value); }
  static auto serdeToReadable(const BigEndian<T> &o) { return o.value; }
  static Result<BigEndian<T>> serdeFrom(size_t value) { return BigEndian<T>{folly::Endian::swap(value)}; }
  static Result<BigEndian<T>> serdeFromReadable(size_t value) { return BigEndian<T>{value}; }
};
