#pragma once

#include <boost/lexical_cast.hpp>
#include <boost/lexical_cast/bad_lexical_cast.hpp>
#include <boost/uuid/nil_generator.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "common/utils/Result.h"
#include "common/utils/String.h"

namespace hf3fs {
struct Uuid : boost::uuids::uuid {
  using is_serde_copyable = void;
  static thread_local boost::uuids::random_generator uuidGenerator;

  static constexpr Uuid zero() { return {{0}}; }
  static Uuid random() { return {uuidGenerator()}; }
  static Uuid max() {
    Uuid uuid;
    memset(uuid.data, 0xff, sizeof(uuid.data));
    return uuid;
  }

  static Uuid from(uint64_t low, uint64_t high) {
    Uuid uuid;
    memcpy(uuid.data, &low, sizeof(low));
    memcpy(uuid.data + sizeof(low), &high, sizeof(high));
    return uuid;
  }

  String toHexString() const { return boost::lexical_cast<String>(*this); }
  std::string serdeToReadable() const { return toHexString(); }

  std::string_view asStringView() const { return std::string_view(reinterpret_cast<const char *>(data), sizeof(data)); }

  static Result<Uuid> fromHexString(std::string_view str) {
    try {
      auto uuid = boost::lexical_cast<boost::uuids::uuid>(str);
      return Uuid{uuid};
    } catch (boost::bad_lexical_cast &ex) {
      return makeError(StatusCode::kDataCorruption, ex.what());
    }
  }

  static Result<Uuid> serdeFromReadable(std::string_view str) { return fromHexString(str); }
};
}  // namespace hf3fs

template <>
struct std::hash<hf3fs::Uuid> {
  size_t operator()(const hf3fs::Uuid &uuid) const;
};

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::Uuid> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::Uuid &uuid, FormatContext &ctx) const {
    return formatter<std::string_view>::format(uuid.toHexString(), ctx);
  }
};

FMT_END_NAMESPACE
