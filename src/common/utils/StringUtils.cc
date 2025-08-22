#include "StringUtils.h"

namespace hf3fs {
String getPrefixEnd(String prefix) {
  int32_t i = prefix.size();
  for (; i > 0; --i) {
    if (prefix[i - 1] != '\xff') {
      ++prefix[i - 1];
      break;
    }
    prefix[i - 1] = '\0';
  }
  if (i == 0) {
    // all \xff
    return {};
  }
  return prefix;
}

String fromHexString(std::string_view c) {
  String to;
  boost::algorithm::unhex(std::begin(c), std::end(c), std::back_inserter(to));
  return to;
}

void encodeOrderPreservedString(String &buf, std::string_view key, bool mayContainNull) {
  if (key.empty()) {
    buf.push_back(0);
    return;
  }
  buf.push_back(1);
  if (!mayContainNull || key.find('\0') == std::string_view::npos) {
    buf.append(key.data(), key.size());
  } else {
    for (char c : key) {
      if (c == 0) {
        buf.push_back(0);
        buf.push_back(0xff);
      } else {
        buf.push_back(c);
      }
    }
  }
  buf.push_back(0);
}

Result<std::string_view> decodeOrderPreservedString(std::string_view key, String &result, bool mayContainNull) {
  result.clear();
  if (key.empty()) {
    return MAKE_ERROR_F(StatusCode::kDataCorruption, "Invalid string key format");
  }
  if (key[0] == 0) {
    // null string
    return key.substr(1);
  }
  if (key[0] != 1) {
    return MAKE_ERROR_F(StatusCode::kDataCorruption, "Invalid string key format");
  }
  // non-null string
  for (size_t i = 1; i < key.size();) {
    if (key[i] == 0) {
      if (mayContainNull && i + 1 < key.size() && key[i + 1] == static_cast<char>(0xff)) {
        result.push_back(0);
        i += 2;
      } else {
        return key.substr(i + 1);
      }
    } else {
      result.push_back(key[i++]);
    }
  }
  return MAKE_ERROR_F(StatusCode::kDataCorruption, "Invalid key format");
}

String encodeOrderPreservedStrings(std::span<const String> keys, bool mayContainNull) {
  String result;
  for (const auto &k : keys) {
    encodeOrderPreservedString(result, k, mayContainNull);
  }
  return result;
}

String encodeOrderPreservedStrings(std::initializer_list<std::string_view> keys, bool mayContainNull) {
  String result;
  for (const auto &k : keys) {
    encodeOrderPreservedString(result, k, mayContainNull);
  }
  return result;
}

Result<std::vector<String>> decodeOrderPreservedStrings(std::string_view key, bool mayContainNull) {
  std::vector<String> result;
  std::string_view remaining = key;
  for (;;) {
    String k;
    auto res = decodeOrderPreservedString(remaining, k, mayContainNull);
    RETURN_ON_ERROR(res);
    remaining = *res;
    result.push_back(std::move(k));
    if (remaining.empty()) {
      break;
    }
  }
  return std::move(result);
}

Result<Void> decodeOrderPreservedStrings(std::string_view key, bool mayContainNull, std::span<String> result) {
  std::string_view remaining = key;
  size_t i = 0;
  for (; i < result.size() && !remaining.empty(); ++i) {
    auto res = decodeOrderPreservedString(remaining, result[i], mayContainNull);
    RETURN_ON_ERROR(res);
    remaining = *res;
  }
  if (i != result.size()) {
    return MAKE_ERROR_F(StatusCode::kDataCorruption,
                        "OrderPreservedString contains less items ({}) than expected ({}): {}",
                        i,
                        result.size(),
                        toHexString(key));
  }
  if (!remaining.empty()) {
    return MAKE_ERROR_F(StatusCode::kDataCorruption,
                        "OrderPreservedString has trailing part: {}. expected {} items",
                        toHexString(key),
                        result.size());
  }
  return Void{};
}
}  // namespace hf3fs
