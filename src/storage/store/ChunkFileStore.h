#pragma once

#include <folly/ThreadLocal.h>
#include <mutex>

#include "common/utils/ConfigBase.h"
#include "common/utils/FdWrapper.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/RobinHood.h"
#include "common/utils/Shards.h"
#include "storage/store/ChunkFileView.h"
#include "storage/store/ChunkMetadata.h"
#include "storage/store/GlobalFileStore.h"
#include "storage/store/PhysicalConfig.h"

namespace hf3fs::storage {

class ChunkFileStore {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_ITEM(preopen_chunk_size_list, std::set<Size>{});
  };

  ChunkFileStore(const Config &config, GlobalFileStore &globalFileStore)
      : config_(config),
        globalFileStore_(globalFileStore) {}

  // create file store.
  Result<Void> create(const PhysicalConfig &config);

  // load file store.
  Result<Void> load(const PhysicalConfig &config);

  // add new chunk size.
  Result<Void> addChunkSize(const std::vector<Size> &sizeList);

  // get a chunk file. [thread-safe]
  Result<ChunkFileView> open(ChunkFileId fileId);

  // recycle a chunk. [thread-safe]
  Result<Void> punchHole(ChunkFileId fileId, size_t offset);

  // allocate space. [thread-safe]
  Result<Void> allocate(ChunkFileId fileId, size_t offset, size_t size);

 protected:
  // open inner file. [thread-safe]
  Result<FileDescriptor *> openInnerFile(ChunkFileId fileId, bool createFile = false);

  // create inner file.
  Result<Void> createInnerFile(Size chunkSize);

 private:
  const Config &config_;
  GlobalFileStore &globalFileStore_;

  Path path_;
  uint32_t physicalFileCount_{};

  constexpr static auto kShardsNum = 64u;
  folly::ThreadLocal<robin_hood::unordered_map<ChunkFileId, FileDescriptor *>> tlsCache_;
};

}  // namespace hf3fs::storage
