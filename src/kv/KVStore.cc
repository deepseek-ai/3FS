#include "kv/KVStore.h"

#include "common/utils/StringUtils.h"
#include "kv/LevelDBStore.h"
#include "kv/MemDBStore.h"
#include "kv/RocksDBStore.h"

namespace hf3fs::kv {

std::unique_ptr<KVStore> KVStore::create(const Config &config, const Options &options) {
  switch (options.type) {
    case Type::LevelDB:
      return LevelDBStore::create(config, options);

    case Type::RocksDB:
      return RocksDBStore::create(config, options);

    case Type::MemDB:
      return std::make_unique<MemDBStore>(config);

    default:
      XLOGF(ERR, "invalid KVStore type: {}", toStringView(options.type));
  }
  return nullptr;
}

}  // namespace hf3fs::kv
