#pragma once
#include "common/utils/ConfigBase.h"

namespace hf3fs::memory {

struct MemoryAllocatorConfig : public ConfigBase<MemoryAllocatorConfig> {
  CONFIG_HOT_UPDATED_ITEM(prof_prefix, "");
  CONFIG_HOT_UPDATED_ITEM(prof_active, bool{});
};

}  // namespace hf3fs::memory
