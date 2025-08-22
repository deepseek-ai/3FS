#pragma once

#include "common/serde/Serde.h"
#include "common/utils/Path.h"
#include "common/utils/Size.h"
#include "kv/KVStore.h"

namespace hf3fs::storage {

// Physical configuration of the storage target. Store in `target.toml`.
static inline constexpr auto kPhysicalConfigFileName = "target.toml";

class PhysicalConfig {
  SERDE_STRUCT_FIELD(path, Path{});
  SERDE_STRUCT_FIELD(target_id, uint64_t{});
  SERDE_STRUCT_FIELD(block_device_uuid, std::string{});
  SERDE_STRUCT_FIELD(allow_disk_without_uuid, false);
  SERDE_STRUCT_FIELD(allow_existing_targets, false);

  SERDE_STRUCT_FIELD(physical_file_count, 256u);
  SERDE_STRUCT_FIELD(chunk_size_list, (std::vector<Size>{512_KB, 1_MB, 2_MB, 4_MB, 16_MB, 64_MB}));
  SERDE_STRUCT_FIELD(chain_id, uint32_t{});
  SERDE_STRUCT_FIELD(kv_store_type, kv::KVStore::Type::LevelDB);
  SERDE_STRUCT_FIELD(has_sentinel, false);
  SERDE_STRUCT_FIELD(kv_store_name, std::string{"meta"});
  SERDE_STRUCT_FIELD(kv_path, std::optional<Path>{});
  SERDE_STRUCT_FIELD(only_chunk_engine, false);

 public:
  Path kvPath() const {
    if (kv_path.has_value()) {
      return *kv_path / fmt::format("{}_{}", kv_store_name, target_id);
    }
    return path / kv_store_name;
  }
};

}  // namespace hf3fs::storage
