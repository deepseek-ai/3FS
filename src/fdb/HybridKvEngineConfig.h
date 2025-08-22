#pragma once

#include "FDBConfig.h"

namespace hf3fs::kv {
struct HybridKvEngineConfig : public ConfigBase<HybridKvEngineConfig> {
  bool operator==(const HybridKvEngineConfig &other) const { return static_cast<const ConfigBase &>(*this) == other; }

  CONFIG_ITEM(use_memkv, false);
  CONFIG_OBJ(fdb, kv::fdb::FDBConfig);
};
}  // namespace hf3fs::kv
