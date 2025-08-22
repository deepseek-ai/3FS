#include "storage/store/ChunkStore.h"

#include <chrono>
#include <fcntl.h>
#include <folly/Hash.h>
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>

#include "common/monitor/Recorder.h"
#include "common/utils/Result.h"
#include "storage/store/ChunkMetadata.h"
#include "storage/store/ChunkReplica.h"
#include "storage/update/UpdateJob.h"

namespace hf3fs::storage {

monitor::OperationRecorder chunkStoreCreateRecorder{"storage.chunk_store_create"};
monitor::OperationRecorder chunkStoreSetRecorder{"storage.chunk_store_set"};
monitor::CountRecorder chunkStoreSetWriteDownRecorder{"storage.chunk_store_set.write_down"};
monitor::CountRecorder chunkStoreSetWriteSkipRecorder{"storage.chunk_store_set.write_skip"};

monitor::OperationRecorder listAllChunkIdsRecorder{"storage.list_all_chunks"};
monitor::DistributionRecorder chunkCountRecorder{"storage.list_all_chunks.chunk_count"};
monitor::DistributionRecorder uncommittedRecorder{"storage.list_all_chunks.uncommitted_count"};

monitor::OperationRecorder queryChunksRecorder{"storage.query_chunks"};
monitor::OperationRecorder resetUncommittedRecorder{"storage.reset_uncommitted"};

// initialize chunk store.
Result<Void> ChunkStore::create(const PhysicalConfig &config) {
  RETURN_AND_LOG_ON_ERROR(fileStore_.create(config));
  RETURN_AND_LOG_ON_ERROR(metaStore_.create(config_.kv_store(), config));
  targetId_ = TargetId{config.target_id};
  tag_ = {{"instance", std::to_string(targetId_)}};
  return Void{};
}

// load chunk store.
Result<Void> ChunkStore::load(const PhysicalConfig &config) {
  RETURN_AND_LOG_ON_ERROR(fileStore_.load(config));
  RETURN_AND_LOG_ON_ERROR(metaStore_.load(config_.kv_store(), config));
  targetId_ = TargetId{config.target_id};
  tag_ = {{"instance", std::to_string(targetId_)}};
  return Void{};
}

// add new chunk size.
Result<Void> ChunkStore::addChunkSize(const std::vector<Size> &sizeList) {
  RETURN_AND_LOG_ON_ERROR(fileStore_.addChunkSize(sizeList));
  RETURN_AND_LOG_ON_ERROR(metaStore_.addChunkSize(sizeList));
  return Void{};
}

Result<ChunkStore::Map::ConstIterator> ChunkStore::get(const ChunkId &chunkId) {
  auto &map_ = maps_[std::hash<ChunkId>{}(chunkId) % kShardsNum];
  // 1. find in cache.
  auto it = map_.find(chunkId);
  if (it != map_.end()) {
    return Result<Map::ConstIterator>(std::move(it));
  }

  // 2. load from DB.
  ChunkInfo chunkInfo;
  auto metaResult = metaStore_.get(chunkId, chunkInfo.meta);
  if (metaResult) {
    auto openResult = fileStore_.open(chunkInfo.meta.innerFileId);
    RETURN_AND_LOG_ON_ERROR(openResult);
    chunkInfo.view = *openResult;
    auto [it, succ] = map_.emplace(chunkId, chunkInfo);
    return Result<Map::ConstIterator>(std::move(it));
  }
  return makeError(std::move(metaResult.error()));
}

Result<Void> ChunkStore::createChunk(const ChunkId &chunkId,
                                     uint32_t chunkSize,
                                     ChunkInfo &chunkInfo,
                                     folly::CPUThreadPoolExecutor &executor,
                                     bool allowToAllocate) {
  auto recordGuard = chunkStoreCreateRecorder.record();
  auto metaResult = metaStore_.createChunk(chunkId, chunkInfo.meta, chunkSize, executor, allowToAllocate);
  RETURN_AND_LOG_ON_ERROR(metaResult);
  auto openResult = fileStore_.open(chunkInfo.meta.innerFileId);
  RETURN_AND_LOG_ON_ERROR(openResult);
  chunkInfo.view = *openResult;
  auto &map_ = maps_[std::hash<ChunkId>{}(chunkId) % kShardsNum];
  map_.insert_or_assign(chunkId, chunkInfo);
  recordGuard.succ();
  return Void{};
}

Result<Void> ChunkStore::set(const ChunkId &chunkId, const ChunkInfo &chunkInfo, bool persist /* = true */) {
  auto recordGuard = chunkStoreSetRecorder.record();
  if (persist || config_.force_persist()) {
    chunkStoreSetWriteDownRecorder.addSample(1);
    auto metaResult = metaStore_.set(chunkId, chunkInfo.meta);
    RETURN_AND_LOG_ON_ERROR(metaResult);
  } else {
    chunkStoreSetWriteSkipRecorder.addSample(1);
  }
  auto &map_ = maps_[std::hash<ChunkId>{}(chunkId) % kShardsNum];
  map_.insert_or_assign(chunkId, chunkInfo);
  recordGuard.succ();
  return Void{};
}

Result<Void> ChunkStore::remove(ChunkId chunkId, ChunkInfo &chunkInfo) {
  if (UNLIKELY(!chunkInfo.meta.readyToRemove())) {
    return makeError(StorageCode::kChunkNotReadyToRemove);
  }
  XLOGF(DBG, "ready to remove: {}", chunkInfo.meta);
  auto getResult = get(chunkId);
  RETURN_AND_LOG_ON_ERROR(getResult);
  auto &map_ = maps_[std::hash<ChunkId>{}(chunkId) % kShardsNum];
  RETURN_AND_LOG_ON_ERROR(metaStore_.remove(chunkId, chunkInfo.meta));
  map_.erase(*getResult);
  return Void{};
}

Result<std::vector<std::pair<ChunkId, ChunkMetadata>>> ChunkStore::queryChunks(const ChunkIdRange &chunkIdRange) {
  auto recordGuard = queryChunksRecorder.record(tag_);

  std::vector<std::pair<ChunkId, ChunkMetadata>> chunkIds;

  auto it = metaStore_.iterator(chunkIdRange.end.data());
  RETURN_AND_LOG_ON_ERROR(it);

  for (; it->valid() && chunkIds.size() < chunkIdRange.maxNumChunkIdsToProcess; it->next()) {
    auto chunkId = it->chunkId();
    auto metadata = it->meta();

    if (chunkId == chunkIdRange.end) {  // [begin, end)
      continue;
    }

    if (chunkId < chunkIdRange.begin) {
      break;
    }

    RETURN_AND_LOG_ON_ERROR(metadata);
    chunkIds.emplace_back(chunkId, *metadata);
  }
  RETURN_AND_LOG_ON_ERROR(it->status());

  recordGuard.succ();
  return chunkIds;
}

Result<Void> ChunkStore::getAllMetadata(ChunkMetaVector &metas) {
  auto recordGuard = listAllChunkIdsRecorder.record(tag_);

  auto it = metaStore_.iterator();
  RETURN_AND_LOG_ON_ERROR(it);

  for (; it->valid(); it->next()) {
    auto chunkId = it->chunkId();
    auto metaResult = it->meta();
    if (UNLIKELY(!metaResult)) {
      XLOGF(ERR, "chunk {} parse meta failed {}", chunkId, metaResult.error());
      return makeError(std::move(metaResult.error()));
    }
    auto &meta = *metaResult;
    metas.emplace_back();
    metas.back().chunkId = std::move(chunkId);
    metas.back().updateVer = meta.updateVer;
    metas.back().commitVer = meta.commitVer;
    metas.back().chainVer = meta.chainVer;
    metas.back().chunkState = meta.chunkState;
    metas.back().checksum = meta.checksum();
    metas.back().length = meta.size;
  }
  RETURN_AND_LOG_ON_ERROR(it->status());

  recordGuard.succ();
  chunkCountRecorder.addSample(metas.size(), tag_);
  return Void{};
}

Result<Void> ChunkStore::resetUncommitted(ChainVer chainVer) {
  auto &uncommitted = metaStore_.uncommitted();
  if (uncommitted.empty()) {
    return Void{};
  }

  XLOGF(CRITICAL, "reset uncommitted chunks, size: {}", uncommitted.size());
  for (auto &chunkId : uncommitted) {
    auto recordGuard = resetUncommittedRecorder.record();
    auto getResult = get(chunkId);
    if (!getResult) {
      XLOGF(ERR, "reset uncommitted chunk {} not found", chunkId);
      continue;
    }

    auto chunkInfo = (*getResult)->second;
    XLOGF(CRITICAL, "reset uncommitted chunk {} meta {}", chunkId, chunkInfo.meta);

    CommitIO commitIO;
    commitIO.key.chunkId = chunkId;
    commitIO.commitVer = chunkInfo.meta.updateVer;
    commitIO.isForce = true;
    commitIO.commitChainVer = chainVer;
    ServiceRequestContext requestCtx{"commit", MessageTag(ClientId{Uuid::max()}, {})};
    ChunkEngineUpdateJob updateChunk{};
    UpdateJob updateJob(requestCtx, commitIO, {}, updateChunk, nullptr);
    auto commitResult = ChunkReplica::commit(*this, updateJob);
    if (!commitResult) {
      XLOGF(ERR, "reset uncommitted chunk {} set failed: {}", chunkId, commitResult.error());
      continue;
    }
    recordGuard.succ();
  }
  uncommitted.clear();
  return Void{};
}

}  // namespace hf3fs::storage
