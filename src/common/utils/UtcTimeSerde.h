#pragma once

#include "UtcTime.h"
#include "common/serde/Serde.h"

namespace hf3fs::serde {

template <>
struct SerdeMethod<UtcTime> {
  static auto serdeTo(UtcTime t) { return t.toMicroseconds(); }
  static Result<UtcTime> serdeFrom(int64_t t) { return UtcTime::fromMicroseconds(t); }
};

}  // namespace hf3fs::serde
