#include "storage/sync/ResyncWorker.h"

#include <folly/ScopeGuard.h>
#include <folly/experimental/coro/Collect.h>

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "fbs/storage/Common.h"
#include "storage/service/Components.h"
#include "storage/update/UpdateJob.h"

namespace hf3fs::storage {
namespace {

monitor::OperationRecorder resyncRecorder{"storage.resync"};
monitor::CountRecorder resyncRoutingVersionMismatch{"storage.resync.routing_version_mismatch"};
monitor::OperationRecorder syncingWriteRecorder{"storage.syncing.write_count"};
monitor::OperationRecorder syncingRemoveRecorder{"storage.syncing.remove_count"};
monitor::CountRecorder syncingSkipCount{"storage.syncing.skip_count"};
monitor::CountRecorder syncingRemoteMissCount{"storage.syncing.remote_miss_count"};
monitor::CountRecorder syncingRemoteChainVersionLowCount{"storage.syncing.chain_version_low"};
monitor::CountRecorder syncingRemoteChainVersionHighCount{"storage.syncing.chain_version_high"};
monitor::CountRecorder syncingLocalUncommittedCount{"storage.syncing.local_uncommitted"};
monitor::CountRecorder syncingCommitVersionMismatchCount{"storage.syncing.commit_version_mismatch"};
monitor::CountRecorder syncingCurrentChainIsWritingCount{"storage.syncing.current_chain_is_writing"};
monitor::CountRecorder syncingRemoteUncommittedCount{"storage.syncing.remote_uncommitted"};
monitor::CountRecorder syncingRemoteFullSyncLightCount{"storage.syncing.full_sync_light"};
monitor::CountRecorder syncingRemoteFullSyncHeavyCount{"storage.syncing.full_sync_heavy"};
monitor::CountRecorder syncingLocalChunkInRecycleState{"storage.syncing.chunk_in_recycle_state"};
monitor::CountRecorder syncingSkipRemoveAfterUpdate{"storage.syncing.skip_remove_after_update"};
monitor::CountRecorder syncingSkipUpdateAfterRemove{"storage.syncing.skip_update_after_remove"};
monitor::ValueRecorder syncingRemainingTargetsCount{"storage.syncing.remaining_targets_count", std::nullopt, false};
monitor::ValueRecorder syncingRemainingChunksCount{"storage.syncing.remaining_chunks_count", std::nullopt, false};

}  // namespace

ResyncWorker::ResyncWorker(const Config &config, Components &components)
    : config_(config),
      components_(components),
      executors_(std::make_pair(config_.num_threads(), config_.num_threads()),
                 std::make_shared<folly::NamedThreadFactory>("Sync")),
      pool_(config_.pool(), &executors_),
      updateChannelAllocator_(config_.num_channels()),
      batchConcurrencyLimiter_(config_.batch_concurrency_limiter()) {}

Result<Void> ResyncWorker::start() {
  RETURN_AND_LOG_ON_ERROR(
      pool_.start([this](VersionedChainId vChainId) -> CoTask<void> { co_await handleSync(vChainId); }));
  executors_.add([this] { loop(); });
  started_ = true;
  return Void{};
}

Result<Void> ResyncWorker::stopAndJoin() {
  stopping_ = true;
  cond_.notify_one();
  for (int i = 0; started_ && !stopped_; ++i) {
    XLOGF_IF(INFO, i % 5 == 0, "Waiting for ResyncWorker@{}::loop stop...", fmt::ptr(this));
    std::this_thread::sleep_for(100_ms);
  }
  pool_.stopAndJoin();
  return Void{};
}

void ResyncWorker::loop() {
  while (!stopping_) {
    auto lock = std::unique_lock(mutex_);
    if (cond_.wait_for(lock, 500_ms, [&] { return stopping_.load(); })) {
      break;
    }

    auto syncingChains = components_.targetMap.snapshot()->syncingChains();
    syncingRemainingTargetsCount.set(syncingChains.size());
    std::shuffle(syncingChains.begin(), syncingChains.end(), std::mt19937{std::random_device{}()});
    for (auto &vChainId : syncingChains) {
      if (stopping_) {
        break;
      }
      bool succ = shards_.withLock(
          [vChainId](SyncingChainIds &syncingChainIds) {
            auto &status = syncingChainIds[vChainId.chainId];
            if (!status.isSyncing && RelativeTime::now() - status.lastSyncingTime > 30_s) {
              status.isSyncing = true;
              return true;
            } else {
              XLOGF(DBG, "chain id {} is syncing", vChainId.chainId);
              return false;
            }
          },
          vChainId.chainId);
      if (succ) {
        pool_.enqueueSync(vChainId);
      }
    }
  }
  stopped_ = true;
  XLOGF(INFO, "ResyncWorker@{}::loop stopped", fmt::ptr(this));
}

CoTryTask<void> ResyncWorker::handleSync(VersionedChainId vChainId) {
  auto fullSyncLevel = config_.full_sync_level();
  auto needFullSync = fullSyncLevel != FullSyncLevel::NONE &&
                      (config_.full_sync_chains().empty() || config_.full_sync_chains().contains(vChainId.chainId));
  bool heavyFullSync = needFullSync && fullSyncLevel == FullSyncLevel::HEAVY;

  // 1. Cancel the syncing state on exit.
  auto guard = folly::makeGuard([&] {
    shards_.withLock(
        [&](SyncingChainIds &syncingChainIds) {
          XLOGF(DBG9, "sync exit chain {}", vChainId);
          auto &status = syncingChainIds[vChainId.chainId];
          status.isSyncing = false;
          status.lastSyncingTime = RelativeTime::now();
        },
        vChainId.chainId);
  });
  XLOGF(DBG9, "start sync chain {}", vChainId);

  // 2. find target and routing.
  auto targetResult = components_.targetMap.getByChainId(vChainId);
  if (UNLIKELY(!targetResult)) {
    auto msg = fmt::format("sync start {} get routing failed: {}", vChainId, targetResult.error());
    XLOG(ERR, msg);
    co_return makeError(StorageCode::kSyncSendStartFailed, std::move(msg));
  }
  auto target = std::move(*targetResult);
  auto targetId = target->targetId;

  ClientId clientId{};
  static_assert(sizeof(ClientId::uuid) == sizeof(VersionedChainId) + sizeof(TargetId));
  *reinterpret_cast<VersionedChainId *>(clientId.uuid.data) = vChainId;
  *reinterpret_cast<TargetId *>(clientId.uuid.data + sizeof(VersionedChainId)) = targetId;

  monitor::TagSet tag;
  tag.addTag("instance", fmt::format("{}-{}", targetId, vChainId.chainVer));
  uint32_t currentSyncingRemoteMissCount = 0;
  uint32_t currentSyncingRemoteChainVersionLowCount = 0;
  uint32_t currentSyncingRemoteChainVersionHighCount = 0;
  uint32_t currentSyncingRemoteUncommittedCount = 0;
  uint32_t currentSyncingLocalUncommittedCount = 0;
  uint32_t currentSyncingCommitVersionMismatchCount = 0;
  uint32_t currentSyncingCurrentChainIsWritingCount = 0;
  uint32_t currentSyncingRemoteFullSyncHeavyCount = 0;
  uint32_t currentSyncingRemoteFullSyncLightCount = 0;
  uint32_t currentSyncingSkipCount = 0;
  auto recordGuard = resyncRecorder.record(tag);

  auto remainingChunksCount = syncingRemainingChunksCount.getRecorderWithTag(tag);
  SCOPE_EXIT { remainingChunksCount->set(0); };

  // 3. sync start.
  net::UserRequestOptions options;
  options.timeout = config_.sync_start_timeout();
  std::vector<ChunkMeta> remoteMetas;

  auto addrResult = target->getSuccessorAddr();
  if (UNLIKELY(!addrResult)) {
    XLOGF(ERR, "sync start get successor addr error: {}", addrResult.error());
    co_return makeError(std::move(addrResult.error()));
  }
  {
    SyncStartReq syncStartReq;
    syncStartReq.vChainId = vChainId;

    auto syncStartResult = co_await components_.messenger.syncStart(*addrResult, syncStartReq, &options);
    if (UNLIKELY(!syncStartResult)) {
      if (syncStartResult.error().code() == StorageClientCode::kRoutingVersionMismatch) {
        recordGuard.dismiss();
        resyncRoutingVersionMismatch.addSample(1);
        auto msg = fmt::format("sync start {} request failed: {}", vChainId, syncStartResult.error());
        XLOG(DBG9, msg);
        co_return makeError(std::move(syncStartResult.error()));
      }
      auto msg = fmt::format("sync start {} request failed: {}", vChainId, syncStartResult.error());
      XLOG(ERR, msg);
      co_return makeError(StorageCode::kSyncSendStartFailed, std::move(msg));
    }

    remoteMetas = std::move(syncStartResult->metas);
  }

  // 3. syncing.
  std::unordered_map<ChunkId, ChunkMetadata> localMetas;
  auto result = target->storageTarget->getAllMetadataMap(localMetas);
  if (UNLIKELY(!result)) {
    XLOGF(ERR, "target invalid iterator {}, error {}", targetId, result.error());
    co_return makeError(std::move(result.error()));
  }
  // re-check current chain version.
  {
    auto targetResult = components_.targetMap.getByChainId(vChainId);
    if (UNLIKELY(!targetResult)) {
      auto msg = fmt::format("sync re-check {} get routing failed: {}", vChainId, targetResult.error());
      XLOG(ERR, msg);
      co_return makeError(StorageCode::kSyncSendStartFailed, std::move(msg));
    }
  }
  std::vector<std::pair<ChunkId, uint32_t>> writeList;
  std::vector<ChunkId> removeList;

  bool hasFatalEvents = false;
  for (auto &remoteMeta : remoteMetas) {
    // 1. check exists.
    auto it = localMetas.find(remoteMeta.chunkId);
    if (it == localMetas.end()) {
      removeList.push_back(remoteMeta.chunkId);
      continue;
    }
    SCOPE_EXIT { localMetas.erase(it); };

    // 2. check recycle state.
    const auto &chunkId = it->first;
    const auto &meta = it->second;
    if (UNLIKELY(meta.recycleState != RecycleState::NORMAL)) {
      XLOGF(WARNING, "target {} chunk {} in recycle state: {}", targetId, chunkId, meta);
      syncingLocalChunkInRecycleState.addSample(1);
      continue;  // skip chunk in recycle state.
    }

    // 3. handle updated write (local == remote).
    bool needForward = true;
    if (meta.chainVer > remoteMeta.chainVer) {
      ++currentSyncingRemoteChainVersionLowCount;
    } else if (remoteMeta.updateVer != remoteMeta.commitVer || remoteMeta.chunkState != ChunkState::COMMIT) {
      XLOGF(WARNING, "chain {} remote uncommitted {}", vChainId.chainId, remoteMeta);
      ++currentSyncingRemoteUncommittedCount;
    } else if (meta.chainVer < remoteMeta.chainVer) {
      if (meta.chunkState == ChunkState::COMMIT) {
        ++currentSyncingRemoteChainVersionHighCount;
        XLOGF(DFATAL, "chain {} remote chain version high, local {}, remote {}", vChainId, meta, remoteMeta);
        hasFatalEvents = true;
        break;
      } else {
        needForward = false;
        ++currentSyncingLocalUncommittedCount;
        XLOGF(CRITICAL, "chain {} local uncommitted, local {}, remote {}", vChainId, meta, remoteMeta);
      }
    } else if (meta.updateVer != remoteMeta.commitVer) {
      if (meta.chainVer != vChainId.chainVer && meta.chunkState == ChunkState::COMMIT) {
        ++currentSyncingCommitVersionMismatchCount;
        XLOGF(DFATAL, "chain {} commit version mismatch, local {}, remote {}", vChainId, meta, remoteMeta);
        hasFatalEvents = true;
        break;
      } else {
        needForward = false;
        ++currentSyncingCurrentChainIsWritingCount;
        XLOGF(CRITICAL, "chain {} chain is writing, local {}, remote {}", vChainId, meta, remoteMeta);
      }
    } else if (heavyFullSync) {
      ++currentSyncingRemoteFullSyncHeavyCount;
    } else if (meta.checksum() != remoteMeta.checksum) {
      if (meta.chainVer != vChainId.chainVer) {
        XLOGF(DFATAL, "chain {} checksum not equal, local {}, remote {}", vChainId, meta, remoteMeta);
        ++currentSyncingRemoteFullSyncLightCount;
        hasFatalEvents = true;
        break;
      } else {
        needForward = false;
        ++currentSyncingCurrentChainIsWritingCount;
        XLOGF(CRITICAL,
              "chain {} checksum not equal because of writing, local {}, remote {}",
              vChainId,
              meta,
              remoteMeta);
      }
    } else {
      needForward = false;
    }
    if (needForward) {
      writeList.emplace_back(chunkId, meta.innerFileId.chunkSize);
    } else {
      ++currentSyncingSkipCount;
    }
  }

  if (UNLIKELY(hasFatalEvents)) {
    auto msg = fmt::format("sync {} has fatal events", vChainId);
    XLOG(CRITICAL, msg);

    OfflineTargetReq req;
    req.targetId = target->successor->targetInfo.targetId;
    req.force = true;
    CO_RETURN_AND_LOG_ON_ERROR(co_await components_.messenger.offlineTarget(*addrResult, req, &options));

    co_return makeError(StorageCode::kSyncSendStartFailed, std::move(msg));
  }

  for (auto &[chunkId, meta] : localMetas) {
    writeList.emplace_back(chunkId, meta.innerFileId.chunkSize);
    ++currentSyncingRemoteMissCount;
  }

  syncingRemoteMissCount.addSample(currentSyncingRemoteMissCount, tag);
  syncingRemoteChainVersionLowCount.addSample(currentSyncingRemoteChainVersionLowCount, tag);
  syncingRemoteChainVersionHighCount.addSample(currentSyncingRemoteChainVersionHighCount, tag);
  syncingLocalUncommittedCount.addSample(currentSyncingLocalUncommittedCount, tag);
  syncingRemoteUncommittedCount.addSample(currentSyncingRemoteUncommittedCount, tag);
  syncingCommitVersionMismatchCount.addSample(currentSyncingCommitVersionMismatchCount, tag);
  syncingCurrentChainIsWritingCount.addSample(currentSyncingCurrentChainIsWritingCount, tag);
  syncingRemoteFullSyncHeavyCount.addSample(currentSyncingRemoteFullSyncHeavyCount, tag);
  syncingRemoteFullSyncLightCount.addSample(currentSyncingRemoteFullSyncLightCount, tag);
  syncingSkipCount.addSample(currentSyncingSkipCount, tag);

  auto batchSize = config_.batch_size();
  auto remainingCount = writeList.size() + removeList.size();
  remainingChunksCount->set(remainingCount);
  for (auto batchStart = 0ul; batchStart < removeList.size(); batchStart += batchSize) {
    auto targetResult = components_.targetMap.getByChainId(vChainId);
    if (UNLIKELY(!targetResult)) {
      auto msg = fmt::format("sync re-check {} get routing failed: {}", vChainId, targetResult.error());
      XLOG(ERR, msg);
      co_return makeError(StorageCode::kSyncSendStartFailed, std::move(msg));
    }
    target = std::move(*targetResult);
    std::vector<CoTryTask<void>> batch;
    for (auto idx = batchStart; idx < removeList.size() && idx < batchStart + batchSize; ++idx) {
      batch.push_back(forward(target, tag, clientId, std::move(removeList[idx]), UpdateType::REMOVE, 0));
    }
    auto guard = batchConcurrencyLimiter_.lock(0);
    auto results = co_await folly::coro::collectAllRange(std::move(batch));
    for (auto &result : results) {
      if (UNLIKELY(!result)) {
        XLOGF(ERR, "target {} forward remove failed {}", targetId, result.error());
        CO_RETURN_ERROR(result);
      }
    }
    remainingCount -= results.size();
    remainingChunksCount->set(remainingCount);
  }
  for (auto batchStart = 0ul; batchStart < writeList.size(); batchStart += batchSize) {
    auto targetResult = components_.targetMap.getByChainId(vChainId);
    if (UNLIKELY(!targetResult)) {
      auto msg = fmt::format("sync re-check {} get routing failed: {}", vChainId, targetResult.error());
      XLOG(ERR, msg);
      co_return makeError(StorageCode::kSyncSendStartFailed, std::move(msg));
    }
    target = std::move(*targetResult);
    std::vector<CoTryTask<void>> batch;
    for (auto idx = batchStart; idx < writeList.size() && idx < batchStart + batchSize; ++idx) {
      auto &[chunkId, chunkSize] = writeList[idx];
      batch.push_back(forward(target, tag, clientId, std::move(chunkId), UpdateType::WRITE, chunkSize));
    }
    auto guard = batchConcurrencyLimiter_.lock(0);
    auto results = co_await folly::coro::collectAllRange(std::move(batch));
    for (auto &result : results) {
      if (UNLIKELY(!result)) {
        XLOGF(ERR, "target {} forward write failed {}", targetId, result.error());
        CO_RETURN_ERROR(result);
      }
    }
    remainingCount -= results.size();
    remainingChunksCount->set(remainingCount);
  }

  // 4. sync done.
  {
    SyncDoneReq syncDoneReq;
    syncDoneReq.vChainId = vChainId;

    auto addrResult = target->getSuccessorAddr();
    if (UNLIKELY(!addrResult)) {
      XLOGF(ERR, "sync start get successor addr error: {}", addrResult.error());
      co_return makeError(std::move(addrResult.error()));
    }
    auto syncDoneResult = co_await components_.messenger.syncDone(*addrResult, syncDoneReq);
    if (UNLIKELY(!syncDoneResult)) {
      auto msg = fmt::format("sync done {} request failed: {}", vChainId, syncDoneResult.error());
      XLOG(ERR, msg);
      co_return makeError(StorageCode::kSyncSendDoneFailed, std::move(msg));
    }
    if (UNLIKELY(!syncDoneResult->result.lengthInfo)) {
      auto msg = fmt::format("sync done {} request failed: {}", vChainId, syncDoneResult->result.lengthInfo.error());
      XLOG(ERR, msg);
      co_return makeError(StorageCode::kSyncSendDoneFailed, std::move(msg));
    }
  }

  recordGuard.succ();
  XLOGF(INFO,
        "sync done chain {} target {} update {} remove {}",
        vChainId,
        targetId,
        writeList.size(),
        removeList.size());
  co_return Void{};
}

CoTryTask<void> ResyncWorker::forward(const TargetPtr &target,
                                      const monitor::TagSet &tag,
                                      const ClientId &clientId,
                                      ChunkId chunkId,
                                      UpdateType updateType,
                                      uint32_t chunkSize) {
  auto recordGuard =
      updateType == UpdateType::REMOVE ? syncingRemoveRecorder.record(tag) : syncingWriteRecorder.record(tag);
  folly::coro::Baton baton;
  auto lockGuard = target->storageTarget->lockChunk(baton, chunkId, "sync");
  if (!lockGuard.locked()) {
    XLOGF(WARNING, "target {} chunk {} wait lock, current tag: {}", *target, chunkId, lockGuard.currentTag());
    co_await lockGuard.lock();
  }

  auto chunkResult = target->storageTarget->queryChunk(chunkId);
  if (chunkResult) {
    // chunk exists.
    if (updateType == UpdateType::REMOVE && chunkResult->recycleState == RecycleState::NORMAL) {
      XLOGF(WARNING, "target {} chunk {} has been updated, skip remove", *target, chunkId);
      syncingSkipRemoveAfterUpdate.addSample(1);
      recordGuard.succ();
      co_return Void{};
    }
    chunkSize = chunkResult->innerFileId.chunkSize;  // use latest chunk size.
  } else if (chunkResult.error().code() == StorageCode::kChunkMetadataNotFound) {
    // chunk does not exist.
    if (updateType == UpdateType::WRITE) {
      XLOGF(WARNING, "target {} chunk {} has been removed, skip updated", *target, chunkId);
      syncingSkipUpdateAfterRemove.addSample(1);
      recordGuard.succ();
      co_return Void{};
    }
  } else {
    co_return makeError(std::move(chunkResult.error()));
  }

  UpdateChannel channel;
  if (UNLIKELY(!updateChannelAllocator_.allocate(channel))) {
    XLOGF(ERR, "no channel to forward sync write");
    co_return makeError(StorageClientCode::kResourceBusy);
  }
  auto channelGuard = folly::makeGuard([&] { updateChannelAllocator_.release(channel); });

  UpdateReq req;
  req.payload.updateType = updateType;
  req.payload.key.chunkId = chunkId;
  req.payload.key.vChainId = target->vChainId;
  req.payload.offset = 0;
  req.payload.chunkSize = chunkSize;
  req.payload.updateVer = ChunkVer{1};
  req.tag.clientId = clientId;
  req.payload.checksum.type = ChecksumType::CRC32C;
  req.tag.requestId = RequestId{++requestId_};
  req.tag.channel = channel;
  req.options.fromClient = false;
  req.options.isSyncing = true;
  req.options.commitChainVer = target->vChainId.chainVer;

  CommitIO commitIO;
  TargetPtr t = target;
  ServiceRequestContext requestCtx{"resync"};
  ChunkEngineUpdateJob chunkEngineJob;
  auto forwardResult =
      co_await components_.reliableForwarding.forwardWithRetry(requestCtx, req, {}, chunkEngineJob, t, commitIO, false);
  CO_RETURN_ON_ERROR(forwardResult.lengthInfo);

  recordGuard.succ();
  co_return Void{};
}

}  // namespace hf3fs::storage
