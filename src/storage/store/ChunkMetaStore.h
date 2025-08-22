#pragma once

#include <folly/AtomicUnorderedMap.h>
#include <memory>
#include <queue>

#include "common/utils/ConfigBase.h"
#include "common/utils/Path.h"
#include "common/utils/UtcTime.h"
#include "kv/KVStore.h"
#include "storage/store/ChunkFileStore.h"
#include "storage/store/ChunkMetadata.h"
#include "storage/store/PhysicalConfig.h"

namespace hf3fs::storage {

class ChunkMetaStore {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(allocate_size, 256_MB, [](Size s) { return s && s % kMaxChunkSize == 0; });
    CONFIG_HOT_UPDATED_ITEM(recycle_batch_size, 256u, ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(punch_hole_batch_size, 16u, ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(removed_chunk_expiration_time, 3_d);
    CONFIG_HOT_UPDATED_ITEM(removed_chunk_force_recycled_time, 1_h);
  };

  ChunkMetaStore(const Config &config, ChunkFileStore &fileStore)
      : config_(config),
        fileStore_(fileStore),
        allocateState_(16) {}

  ~ChunkMetaStore();

  // create chunk meta store.
  Result<Void> create(const kv::KVStore::Config &config, const PhysicalConfig &targetConfig);

  // load chunk meta store.
  Result<Void> load(const kv::KVStore::Config &config,
                    const PhysicalConfig &targetConfig,
                    bool createIfMissing = false);

  // add new chunk size.
  Result<Void> addChunkSize(const std::vector<Size> &sizeList);

  // migrate chunk meta store.
  Result<Void> migrate(const kv::KVStore::Config &config, const PhysicalConfig &targetConfig);

  // get metadata of chunk. [thread-safe]
  Result<Void> get(const ChunkId &chunkId, ChunkMetadata &meta);

  // set metadata of chunk. [thread-safe]
  Result<Void> set(const ChunkId &chunkId, const ChunkMetadata &meta);

  // remove metadata of chunk. [thread-safe]
  Result<Void> remove(const ChunkId &chunkId, const ChunkMetadata &meta);

  // create a chunk. [thread-safe]
  Result<Void> createChunk(const ChunkId &chunkId,
                           ChunkMetadata &meta,
                           uint32_t chunkSize,
                           folly::CPUThreadPoolExecutor &executor,
                           bool allowToAllocate);

  // recycle a batch of chunks, return true if has more. [thread-safe]
  Result<bool> punchHole();

  // sync the LOG of kv.
  Result<Void> sync();

  // get used size.
  uint64_t usedSize() const { return std::max(int64_t(createdSize_.load() - removedSize_.load()), 0l); }

  // get reserved and unrecycled size.
  Result<Void> unusedSize(int64_t &reservedSize, int64_t &unrecycledSize);

  // get all uncommitted chunk ids.
  auto &uncommitted() { return uncommitted_; }

  // enable or disable emergency recycling.
  void setEmergencyRecycling(bool enable) { emergencyRecycling_ = enable; }

  // iterator.
  class Iterator {
   public:
    explicit Iterator(kv::KVStore::IteratorPtr it, std::string_view chunkIdPrefix);
    // seek a chunk id prefix.
    void seek(std::string_view chunkIdPrefix);
    // return valid or not.
    bool valid() const;
    // get current chunk id.
    ChunkId chunkId() const;
    // get current metadata.
    Result<ChunkMetadata> meta() const;
    // next metadata.
    void next();
    // check status.
    Result<Void> status() const;

   private:
    kv::KVStore::IteratorPtr it_;
  };
  Result<Iterator> iterator(std::string_view chunkIdPrefix = {});

 protected:
  Result<Void> checkSentinel(std::string_view key);

  Result<Void> getSize(std::string_view key, std::atomic<uint64_t> &size);

  struct AllocateState {
    std::mutex createMutex;
    std::mutex recycleMutex;
    std::mutex allocateMutex;
    std::atomic<bool> loaded{};
    std::atomic<bool> allocating{};
    std::atomic<bool> recycling{};
    uint32_t chunkSize{};
    uint32_t allocateIndex{};               // createMutex.
    std::atomic<uint64_t> startingPoint{};  // createMutex.
    std::atomic<uint64_t> createdCount{};   // createMutex.
    std::atomic<uint64_t> usedCount{};      // createMutex.
    std::atomic<uint64_t> removedCount{};
    std::atomic<uint64_t> recycledCount{};                 // recycleMutex
    std::atomic<uint64_t> reusedCount{};                   // createMutex
    std::atomic<uint64_t> holeCount{};                     // recycleMutex
    std::atomic<UtcTime> oldestRemovedTimestamp{};         // recycleMutex
    std::vector<ChunkPosition> createdChunks;              // createMutex
    std::vector<ChunkPosition> recycledChunks;             // createMutex
    robin_hood::unordered_map<uint32_t, size_t> fileSize;  // createMutex
  };
  void createAllocateState(uint32_t chunkSize);

  Result<AllocateState *> loadAllocateState(uint32_t chunkSize);

  Result<Void> allocateChunks(AllocateState &state, bool withLock = false);

  bool needRecycleRemovedChunks(AllocateState &state);

  Result<Void> recycleRemovedChunks(AllocateState &state, bool withLock = false);

  Result<bool> punchHoleRemovedChunks(AllocateState &state, uint64_t expirationUs);

 private:
  const Config &config_;
  ChunkFileStore &fileStore_;

  std::unique_ptr<kv::KVStore> kv_;
  std::string sentinel_;
  std::string kvName_;
  bool hasSentinel_ = false;
  uint32_t physicalFileCount_ = 256;

  std::atomic<uint64_t> createdSize_ = 0;
  std::atomic<uint64_t> removedSize_ = 0;
  std::vector<ChunkId> uncommitted_;

  std::atomic<bool> emergencyRecycling_ = false;

  folly::AtomicUnorderedInsertMap<uint32_t, std::unique_ptr<AllocateState>> allocateState_;
};

}  // namespace hf3fs::storage
