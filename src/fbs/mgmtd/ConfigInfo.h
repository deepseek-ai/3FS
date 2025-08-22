#pragma once

#include "MgmtdTypes.h"
#include "common/serde/SerdeHelper.h"

namespace hf3fs::flat {
struct ConfigInfo : public serde::SerdeHelper<ConfigInfo> {
  SERDE_STRUCT_FIELD(configVersion, ConfigVersion(0));
  SERDE_STRUCT_FIELD(content, String{});
  SERDE_STRUCT_FIELD(desc, String{});

 public:
  String genUpdateDesc() const { return fmt::format("version: {} desc: {}", configVersion.toUnderType(), desc); }
};
}  // namespace hf3fs::flat
