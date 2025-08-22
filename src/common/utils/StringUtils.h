#pragma once

#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string.hpp>
#include <fmt/format.h>
#include <span>

#include "Result.h"
#include "String.h"
#include "common/utils/MagicEnum.hpp"

namespace hf3fs {
template <typename T>
requires(std::is_enum_v<T>) inline String toString(T t) { return String(magic_enum::enum_name(t)); }

template <typename T>
requires(std::is_enum_v<T>) inline std::string_view toStringView(T t) { return magic_enum::enum_name(t); }

template <typename Container>
String toHexString(const Container &c) {
  String to;
  boost::algorithm::hex(std::begin(c), std::end(c), std::back_inserter(to));
  return to;
}

String fromHexString(std::string_view c);

auto splitAndTransform(std::string_view src, auto &&delimiterPredicte, auto &&transform) {
  using RetType = std::decay_t<decltype(transform(src))>;
  std::vector<std::string_view> slices;
  boost::split(slices, src, std::forward<decltype(delimiterPredicte)>(delimiterPredicte));
  std::vector<RetType> result;
  result.reserve(slices.size());
  for (auto slice : slices) {
    if (!slice.empty()) {
      result.push_back(transform(slice));
    }
  }
  return result;
}

template <typename T>
using StringMap = std::map<String, T, std::less<>>;

// return empty string if prefix is all of \xff
String getPrefixEnd(String prefix);

void encodeOrderPreservedString(String &buf, std::string_view key, bool mayContainNull);
Result<std::string_view> decodeOrderPreservedString(std::string_view key, String &result, bool mayContainNull);

String encodeOrderPreservedStrings(std::span<const String> keys, bool mayContainNull);
String encodeOrderPreservedStrings(std::initializer_list<std::string_view> keys, bool mayContainNull);
Result<std::vector<String>> decodeOrderPreservedStrings(std::string_view key, bool mayContainNull);
Result<Void> decodeOrderPreservedStrings(std::string_view key, bool mayContainNull, std::span<String> result);

}  // namespace hf3fs
