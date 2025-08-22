#pragma once
#include <cassert>
#include <fmt/format.h>
#include <string_view>
#include <type_traits>
#include <utility>

#include "common/utils/Conversion.h"
#include "common/utils/Result.h"
#include "common/utils/String.h"

namespace hf3fs {

namespace detail {
template <typename T, typename... Ts>
constexpr inline size_t totalSize(const T &, const Ts &...others) {
  size_t total = sizeof(T);
  if constexpr (sizeof...(others) > 0) {
    total += totalSize(others...);
  }
  return total;
}
};  // namespace detail

struct Serializer {
  explicit Serializer(String &s)
      : s_(s) {}

  template <typename... Args>
  static String serRawArgs(Args &&...args) {
    String buf;
    buf.reserve(detail::totalSize(std::forward<Args>(args)...));
    Serializer ser(buf);
    ser.put(std::forward<Args>(args)...);
    return buf;
  }

  Serializer &putChar(char c) {
    s_.push_back(c);
    return *this;
  }

  template <typename T>
  Serializer &putIntAsChar(T v) {
    assert(isSafeConvertTo<char>(v));
    return putChar(static_cast<char>(v));
  }

  // for strings whose length is less than 128.
  Serializer &putShortString(std::string_view s) {
    putIntAsChar(s.size());
    s_.append(s.data(), s.size());
    return *this;
  }

  template <typename T>
  Serializer &put(const T &v) {
    static_assert(std::is_trivial_v<T>);
    return putRaw(&v, sizeof(T));
  }

  template <typename T, typename... Ts>
  Serializer &put(const T &v, const Ts &...others) {
    put(v);
    if constexpr (sizeof...(others) > 0) {
      put(others...);
    }
    return *this;
  }

  Serializer &putRaw(const void *data, size_t length) {
    s_.append(reinterpret_cast<const char *>(data), length);
    return *this;
  }

  String &s_;
};

struct Deserializer {
  explicit Deserializer(std::string_view s, size_t pos = 0)
      : s_(s),
        pos_(pos) {}

  template <typename... Args>
  static Result<Void> deserRawArgs(std::string_view s, Args &...args) {
    Deserializer des(s);
    RETURN_ON_ERROR(des.get(args...));
    if (!des.reachEnd()) {
      return makeError(StatusCode::kDataCorruption,
                       fmt::format("Data Corruption when deserialize at pos {}: should reach end, remaining {}.",
                                   des.pos_,
                                   des.s_.size() - des.pos_));
    }
    return Void();
  }

#define CHECK_BUFFER_LENGTH(n)                                                                       \
  do {                                                                                               \
    auto nn = (n);                                                                                   \
    if (pos_ + nn > s_.size()) {                                                                     \
      String msg = fmt::format("Data Corruption when deserialize at pos {}: need {}, remaining {}.", \
                               pos_,                                                                 \
                               nn,                                                                   \
                               s_.size() - pos_);                                                    \
      return folly::makeUnexpected(Status(StatusCode::kDataCorruption, std::move(msg)));             \
    }                                                                                                \
  } while (false)

  Result<char> getChar() noexcept {
    CHECK_BUFFER_LENGTH(1);
    return s_[pos_++];
  }

  template <typename T>
  Result<T> getIntFromChar() noexcept {
    return getChar().then([](char c) {
      assert(isSafeConvertTo<T>(c));
      return static_cast<T>(c);
    });
  }

  Result<std::string_view> getShortString() noexcept {
    return getIntFromChar<size_t>().then([this](size_t size) { return getRaw(size); });
  }

  template <typename T>
  Result<T> get() noexcept {
    static_assert(std::is_trivial_v<T>);
    CHECK_BUFFER_LENGTH(sizeof(T));
    T val;
    std::memcpy(&val, &s_[pos_], sizeof(T));
    pos_ += sizeof(T);
    return val;
  }

  template <typename T, typename... Ts>
  Result<Void> get(T &t, Ts &...others) {
    auto result = get<T>();
    RETURN_ON_ERROR(result);
    t = *result;
    if constexpr (sizeof...(others) > 0) {
      return get(others...);
    }
    return Void{};
  }

  Result<std::string_view> getRaw(size_t length) noexcept {
    CHECK_BUFFER_LENGTH(length);
    std::string_view res(&s_[pos_], length);
    pos_ += length;
    return res;
  }

  Result<std::string_view> getRawUntilEnd() noexcept {
    std::string_view res(&s_[pos_], s_.length() - pos_);
    pos_ = s_.length();
    return res;
  }

#undef CHECK_BUFFER_LENGTH

  bool reachEnd() const noexcept { return pos_ >= s_.size(); }

  std::string_view s_;
  size_t pos_ = 0;
};

}  // namespace hf3fs
