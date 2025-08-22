#pragma once

#include "common/serde/Serde.h"
#include "common/utils/UtcTime.h"

namespace hf3fs::app {
struct ConfigUpdateRecord {
  SERDE_STRUCT_FIELD(updateTime, UtcTime{});
  SERDE_STRUCT_FIELD(result, Status(StatusCode::kOK));
  SERDE_STRUCT_FIELD(description, String{});
};
}  // namespace hf3fs::app
