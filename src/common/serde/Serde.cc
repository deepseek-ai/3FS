#include "common/serde/Serde.h"

#include <exception>
#include <folly/dynamic.h>
#include <folly/json.h>
#include <sstream>
#include <type_traits>
#include <utility>

#include "common/serde/TypeName.h"
#include "common/utils/MagicEnum.hpp"
#include "toml.hpp"

namespace hf3fs::serde {

struct JsonObject : public folly::dynamic {
  using dynamic::dynamic;
};
struct TomlObject : public toml::value {
  using basic_value::basic_value;
};

template <>
Out<JsonObject>::Out() = default;
template <>
Out<JsonObject>::~Out() = default;

template <>
int Out<JsonObject>::tableBegin(bool) {
  root_.push_back(JsonObject{JsonObject::object});
  return 0;
}
template <>
void Out<JsonObject>::arrayBegin() {
  root_.push_back(JsonObject{JsonObject::array});
}

template <>
void Out<JsonObject>::end() {
  if (root_.size() > 1) {
    auto value = std::move(root_.back());
    root_.pop_back();
    add(std::move(value));
  }
}

template <>
void Out<JsonObject>::value(Optional has) {
  if (has == Optional::NullOpt) {
    add(nullptr);
  }
}

template <>
template <>
void Out<JsonObject>::add(auto &&v) {
  if (inArray()) {  // in array.
    root_.back().push_back(std::forward<decltype(v)>(v));
  } else if (inTable()) {  // in table.
    auto key = std::move(root_.back());
    root_.pop_back();
    root_.back().insert(std::move(key), std::forward<decltype(v)>(v));
  } else {
    root_.push_back(std::forward<decltype(v)>(v));
  }
}

template <>
bool Out<JsonObject>::inTable() {
  return root_.size() >= 2 && root_[root_.size() - 2].isObject();
}
template <>
bool Out<JsonObject>::inArray() {
  return !root_.empty() && root_.back().isArray();
}

template <>
std::string Out<JsonObject>::toString(bool sortKeys, bool prettyFormatting) {
  folly::json::serialization_opts opts;
  opts.sort_keys = sortKeys;
  opts.pretty_formatting = prettyFormatting;
  return root_.empty() ? "" : folly::json::serialize(root_.front(), opts);
}

template <>
Out<TomlObject>::Out() = default;
template <>
Out<TomlObject>::~Out() = default;

template <>
int Out<TomlObject>::tableBegin(bool) {
  root_.emplace_back(TomlObject::table_type{});
  return 0;
}
template <>
void Out<TomlObject>::arrayBegin() {
  root_.emplace_back(TomlObject::array_type{});
}

template <>
template <>
void Out<TomlObject>::add(auto &&v) {
  using T = decltype(v);
  if (inArray()) {  // in array.
    root_.back().push_back(std::forward<T>(v));
  } else if (inTable()) {  // in table.
    auto key = std::move(root_.back().as_string().str);
    root_.pop_back();
    root_.back()[key] = std::forward<T>(v);
  } else {
    root_.emplace_back(std::forward<T>(v));
  }
}

template <>
bool Out<TomlObject>::inTable() {
  return root_.size() >= 2 && root_[root_.size() - 2].is_table();
}
template <>
bool Out<TomlObject>::inArray() {
  return !root_.empty() && root_.back().is_array();
}

template <>
void Out<TomlObject>::end() {
  if (root_.size() > 1) {
    auto value = std::move(root_.back());
    root_.pop_back();
    add(std::move(value));
  }
}

template <>
void Out<TomlObject>::value(Optional has) {
  if (has == Optional::NullOpt && inTable()) {
    root_.pop_back();
  }
}

template <>
std::string Out<TomlObject>::toString(bool /*sortKeys*/, bool /*prettyFormatting*/) {
  if (root_.empty()) {
    return "";
  }
  std::stringstream ss;
  ss << toml::format(root_.front(), 80, std::numeric_limits<toml::floating>::max_digits10, false, true);
  return ss.str();
}

template class Out<JsonObject>;
template class Out<TomlObject>;

template <>
In<JsonObject>::In(const folly::dynamic &obj)
    : obj_(static_cast<const JsonObject &>(obj)) {}

Result<Void> In<JsonObject>::parse(std::string_view str, std::function<Result<Void>(const JsonObject &)> func) {
  try {
    auto dynamic = folly::parseJson(str);
    if (dynamic.isNull()) {
      return MAKE_ERROR_F(StatusCode::kSerdeInvalidJson, "Json is invalid: {}", str);
    }
    return func(static_cast<const JsonObject &>(dynamic));
  } catch (const std::exception &e) {
    return MAKE_ERROR_F(StatusCode::kSerdeInvalidJson, "Json is invalid: {}", e.what());
  }
  return MAKE_ERROR_F(StatusCode::kSerdeInvalidJson, "Json is invalid");
}

Result<In<JsonObject>> In<JsonObject>::parseKey(std::string_view key) const {
  if (obj_.count(key)) {
    return In(static_cast<const JsonObject &>(obj_.at(key)));
  }
  return makeError(StatusCode::kSerdeKeyNotFound, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

Result<In<JsonObject>> In<JsonObject>::parseTable() const {
  if (obj_.isObject()) {
    return *this;
  }
  return makeError(StatusCode::kSerdeNotTable, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

Result<bool> In<JsonObject>::parseBoolean() const {
  if (obj_.isBool()) {
    return obj_.getBool();
  }
  return makeError(StatusCode::kSerdeNotInteger, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

Result<int64_t> In<JsonObject>::parseInteger() const {
  if (obj_.isInt()) {
    return obj_.getInt();
  }
  return makeError(StatusCode::kSerdeNotInteger, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

Result<double> In<JsonObject>::parseFloat() const {
  if (obj_.isNumber()) {
    return obj_.asDouble();
  }
  return makeError(StatusCode::kSerdeNotNumber, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

Result<std::string_view> In<JsonObject>::parseString() const {
  if (obj_.isString()) {
    return std::string_view{obj_.getString()};
  }
  return makeError(StatusCode::kSerdeNotString, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

Result<std::pair<std::string_view, In<JsonObject>>> In<JsonObject>::parseVariant() const {
  RETURN_AND_LOG_ON_ERROR(parseTable());
  auto keyResult = parseKey("type");
  RETURN_AND_LOG_ON_ERROR(keyResult);
  std::string_view typeName;
  RETURN_AND_LOG_ON_ERROR(deserialize(typeName, *keyResult));
  auto valueResult = parseKey("value");
  RETURN_AND_LOG_ON_ERROR(valueResult);
  return std::make_pair(typeName, *valueResult);
}

Result<Void> In<JsonObject>::parseOptional(Optional &optional) const {
  optional = obj_.isNull() ? Optional::NullOpt : Optional::HasValue;
  return Void{};
}

Result<std::pair<uint32_t, std::unique_ptr<void, void (*)(void *)>>> In<JsonObject>::parseContainer() const {
  if (obj_.isObject()) {
    using It = decltype(obj_.items().begin());
    auto it = ObjectPool<It>::get(obj_.items().begin());
    return std::pair<uint32_t, std::unique_ptr<void, void (*)(void *)>>{
        obj_.size(),
        std::unique_ptr<void, void (*)(void *)>{reinterpret_cast<void *>(it.release()),
                                                [](void *p) { ObjectPool<It>::Ptr{reinterpret_cast<It *>(p)}; }}};
  } else if (obj_.isArray()) {
    using It = decltype(obj_.begin());
    auto it = ObjectPool<It>::get(obj_.begin());
    return std::pair<uint32_t, std::unique_ptr<void, void (*)(void *)>>{
        obj_.size(),
        std::unique_ptr<void, void (*)(void *)>{reinterpret_cast<void *>(it.release()),
                                                [](void *p) { ObjectPool<It>::Ptr{reinterpret_cast<It *>(p)}; }}};
  }
  return makeError(StatusCode::kSerdeNotContainer, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

In<JsonObject> In<JsonObject>::fetchAndNext(std::unique_ptr<void, void (*)(void *)> &it) const {
  using It = decltype(obj_.begin());
  auto &realIt = *reinterpret_cast<It *>(it.get());
  In in(*realIt);
  ++realIt;
  return in;
}

Result<std::string_view> In<JsonObject>::fetchKey(std::unique_ptr<void, void (*)(void *)> &it) const {
  using It = decltype(obj_.items().begin());
  auto &realIt = *reinterpret_cast<It *>(it.get());
  if (UNLIKELY(!realIt->first.isString())) {
    return makeError(StatusCode::kSerdeNotString, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
  }
  return std::string_view{realIt->first.getString()};
}

In<JsonObject> In<JsonObject>::fetchValue(std::unique_ptr<void, void (*)(void *)> &it) const {
  using It = decltype(obj_.items().begin());
  auto &realIt = *reinterpret_cast<It *>(it.get());
  return In(realIt->second);
}

void In<JsonObject>::next(std::unique_ptr<void, void (*)(void *)> &it) const {
  using It = decltype(obj_.items().begin());
  auto &realIt = *reinterpret_cast<It *>(it.get());
  ++realIt;
}

template <>
In<TomlObject>::In(const toml::value &obj)
    : obj_(static_cast<const TomlObject &>(obj)) {}

Result<Void> In<TomlObject>::parse(std::string_view str, std::function<Result<Void>(const TomlObject &)> func) {
  std::istringstream in{std::string{str}};
  toml::value value;
  try {
    value = toml::parse(in);
  } catch (const std::exception &e) {
    return makeError(StatusCode::kSerdeInvalidToml, "Toml is invalid: {}", e.what());
  }
  return func(static_cast<const TomlObject &>(value));
}

Result<Void> In<TomlObject>::parseFile(const Path &path, std::function<Result<Void>(const TomlObject &)> func) {
  toml::value value;
  try {
    value = toml::parse(path.string());
  } catch (const std::exception &e) {
    return makeError(StatusCode::kSerdeInvalidToml,
                     fmt::format("Toml file is invalid: {}, path {}", e.what(), path.string()));
  }
  return func(static_cast<const TomlObject &>(value));
}

Result<In<TomlObject>> In<TomlObject>::parseKey(std::string_view keyIn) const {
  std::string key{keyIn};
  if (obj_.count(key)) {
    return In(static_cast<const TomlObject &>(obj_.at(key)));
  }
  return makeError(StatusCode::kSerdeKeyNotFound, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

Result<In<TomlObject>> In<TomlObject>::parseTable() const {
  if (obj_.is_table()) {
    return *this;
  }
  return makeError(StatusCode::kSerdeNotTable, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

Result<bool> In<TomlObject>::parseBoolean() const {
  if (obj_.is_boolean()) {
    return obj_.as_boolean(std::nothrow);
  }
  return makeError(StatusCode::kSerdeNotInteger, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

Result<int64_t> In<TomlObject>::parseInteger() const {
  if (obj_.is_integer()) {
    return obj_.as_integer(std::nothrow);
  }
  return makeError(StatusCode::kSerdeNotInteger, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

Result<double> In<TomlObject>::parseFloat() const {
  if (obj_.is_floating()) {
    return obj_.as_floating(std::nothrow);
  }
  if (obj_.is_integer()) {
    return obj_.as_integer(std::nothrow);
  }
  return makeError(StatusCode::kSerdeNotNumber, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

Result<std::string_view> In<TomlObject>::parseString() const {
  if (obj_.is_string()) {
    return std::string_view{obj_.as_string(std::nothrow)};
  }
  return makeError(StatusCode::kSerdeNotString, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

Result<std::pair<std::string_view, In<TomlObject>>> In<TomlObject>::parseVariant() const {
  RETURN_AND_LOG_ON_ERROR(parseTable());
  auto keyResult = parseKey("type");
  RETURN_AND_LOG_ON_ERROR(keyResult);
  std::string_view typeName;
  RETURN_AND_LOG_ON_ERROR(deserialize(typeName, *keyResult));
  auto valueResult = parseKey("value");
  RETURN_AND_LOG_ON_ERROR(valueResult);
  return std::make_pair(typeName, *valueResult);
}

Result<Void> In<TomlObject>::parseOptional(Optional &optional) const {
  optional = obj_.is_uninitialized() ? Optional::NullOpt : Optional::HasValue;
  return Void{};
}

Result<std::pair<uint32_t, std::unique_ptr<void, void (*)(void *)>>> In<TomlObject>::parseContainer() const {
  if (obj_.is_table()) {
    using It = decltype(obj_.as_table(std::nothrow).begin());
    auto it = ObjectPool<It>::get(obj_.as_table(std::nothrow).begin());
    return std::pair<uint32_t, std::unique_ptr<void, void (*)(void *)>>{
        obj_.size(),
        std::unique_ptr<void, void (*)(void *)>{reinterpret_cast<void *>(it.release()),
                                                [](void *p) { ObjectPool<It>::Ptr{reinterpret_cast<It *>(p)}; }}};
  } else if (obj_.is_array()) {
    using It = decltype(obj_.as_array(std::nothrow).begin());
    auto it = ObjectPool<It>::get(obj_.as_array(std::nothrow).begin());
    return std::pair<uint32_t, std::unique_ptr<void, void (*)(void *)>>{
        obj_.size(),
        std::unique_ptr<void, void (*)(void *)>{reinterpret_cast<void *>(it.release()),
                                                [](void *p) { ObjectPool<It>::Ptr{reinterpret_cast<It *>(p)}; }}};
  }
  return makeError(StatusCode::kSerdeNotContainer, fmt::format("type is {}", magic_enum::enum_name(obj_.type())));
}

In<TomlObject> In<TomlObject>::fetchAndNext(std::unique_ptr<void, void (*)(void *)> &it) const {
  using It = decltype(obj_.as_array(std::nothrow).begin());
  auto &realIt = *reinterpret_cast<It *>(it.get());
  In in(*realIt);
  ++realIt;
  return in;
}

Result<std::string_view> In<TomlObject>::fetchKey(std::unique_ptr<void, void (*)(void *)> &it) const {
  using It = decltype(obj_.as_table(std::nothrow).begin());
  auto &realIt = *reinterpret_cast<It *>(it.get());
  return std::string_view{realIt->first};
}

In<TomlObject> In<TomlObject>::fetchValue(std::unique_ptr<void, void (*)(void *)> &it) const {
  using It = decltype(obj_.as_table(std::nothrow).begin());
  auto &realIt = *reinterpret_cast<It *>(it.get());
  return In(realIt->second);
}

void In<TomlObject>::next(std::unique_ptr<void, void (*)(void *)> &it) const {
  using It = decltype(obj_.as_table(std::nothrow).begin());
  auto &realIt = *reinterpret_cast<It *>(it.get());
  ++realIt;
}

}  // namespace hf3fs::serde
