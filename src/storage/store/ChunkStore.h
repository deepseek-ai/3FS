#pragma once

#include <folly/concurrency/ConcurrentHashMap.h>
#include <optional>
#include <vector>

#include "common/monitor/Recorder.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/FdWrapper.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/RobinHood.h"
#include "storage/store/ChunkFileStore.h"
#include "storage/store/ChunkMetaStore.h"
#include "storage/store/ChunkMetadata.h"

namespace hf3fs::storage {

enum class PointQueryStrategy {
  NONE,
  CLASSIC,
  MODERN,
};

class ChunkStore {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_OBJ(kv_store, kv::KVStore::Config);
    CONFIG_OBJ(file_store, ChunkFileStore::Config);
    CONFIG_OBJ(meta_store, ChunkMetaStore::Config);
    CONFIG_ITEM(mutex_num, 257u, ConfigCheckers::isPositivePrime<uint32_t>);
    CONFIG_ITEM(kv_path, Path{});
    CONFIG_HOT_UPDATED_ITEM(migrate_kv_store, false);
    CONFIG_HOT_UPDATED_ITEM(force_persist, true);
    CONFIG_HOT_UPDATED_ITEM(point_query_strategy, PointQueryStrategy::NONE);
  };

  using Map = folly::ConcurrentHashMap<ChunkId, ChunkInfo>;

  ChunkStore(const Config &config, GlobalFileStore &globalFileStore)
      : config_(config),
        fileStore_(config_.file_store(), globalFileStore),
        metaStore_(config_.meta_store(), fileStore_) {}

  ChunkMetaStore &chunkMetaStore() { return metaStore_; }

  // create chunk store.
  Result<Void> create(const PhysicalConfig &config);

  // load chunk store.
  Result<Void> load(const PhysicalConfig &config);

  // add new chunk size.
  Result<Void> addChunkSize(const std::vector<Size> &sizeList);

  // migrate meta store.
  Result<Void> migrate(const PhysicalConfig &config) { return metaStore_.migrate(config_.kv_store(), config); }

  // get meta of a chunk file.
  Result<Map::ConstIterator> get(const ChunkId &chunkId);

  // create a new chunk file.
  Result<Void> createChunk(const ChunkId &chunkId,
                           uint32_t chunkSize,
                           ChunkInfo &chunkInfo,
                           folly::CPUThreadPoolExecutor &executor,
                           bool allowToAllocate);

  // set meta of a chunk file.
  Result<Void> set(const ChunkId &chunkId, const ChunkInfo &chunkInfo, bool persist = true);

  // remove a chunk file.
  Result<Void> remove(ChunkId chunkId, ChunkInfo &chunkInfo);

  // recycle a batch of chunks.
  Result<bool> punchHole() { return metaStore_.punchHole(); }

  // sync meta kv.
  Result<Void> sync() { return metaStore_.sync(); }

  // query chunks: the chunk ids in result are in reverse lexicographical order
  Result<std::vector<std::pair<ChunkId, ChunkMetadata>>> queryChunks(const ChunkIdRange &chunkIdRange);

  // list all chunk ids.
  Result<Void> getAllMetadata(ChunkMetaVector &metas);

  // get meta iterator.
  auto metaIterator() { return metaStore_.iterator(); }

  // get used size.
  uint64_t usedSize() const { return metaStore_.usedSize(); }

  // get reserved and unrecycled size.
  Result<Void> unusedSize(int64_t &reservedSize, int64_t &unrecycledSize) {
    return metaStore_.unusedSize(reservedSize, unrecycledSize);
  }

  // get all uncommitted chunk ids.
  const auto &uncommitted() { return metaStore_.uncommitted(); }

  // reset uncommitted chunk to committed state.
  Result<Void> resetUncommitted(ChainVer chainVer);

  // enable or disable emergency recycling.
  void setEmergencyRecycling(bool enable) { return metaStore_.setEmergencyRecycling(enable); }

 private:
  const Config &config_;
  ChunkFileStore fileStore_;
  ChunkMetaStore metaStore_;
  TargetId targetId_;
  monitor::TagSet tag_;
  static constexpr auto kShardsNum = 32u;
  std::array<Map, kShardsNum> maps_;
};

}  // namespace hf3fs::storage
