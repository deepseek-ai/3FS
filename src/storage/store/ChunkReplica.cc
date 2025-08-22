#include "storage/store/ChunkReplica.h"

#include <folly/Random.h>
#include <folly/logging/xlog.h>

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "common/utils/FileUtils.h"
#include "common/utils/Result.h"
#include "common/utils/SysResource.h"
#include "fbs/storage/Common.h"
#include "scn/scan/scan.h"
#include "storage/store/ChunkMetadata.h"

namespace hf3fs::storage {
namespace {

std::array<uint8_t, kMaxChunkSize> kZeroBytes{};

monitor::OperationRecorder storageAioReadRecorder{"storage.chunk_read"};
monitor::CountRecorder storageReadUncommitted{"storage.chunk_read.uncommitted"};

monitor::OperationRecorder storageUpdateRecorder{"storage.chunk_update"};
monitor::CountRecorder storageUpdateSyncSmallerVersion{"storage.chunk_update.sync_smaller_version"};
monitor::CountRecorder storageUpdateChecksumReadChunk{"storage.chunk_update.checksum_read_chunk"};
monitor::CountRecorder storageUpdateChecksumCombine{"storage.chunk_update.checksum_combine"};
monitor::CountRecorder storageUpdateChecksumReuse{"storage.chunk_update.checksum_reuse"};
monitor::CountRecorder storageUpdateChecksumNone{"storage.chunk_update.checksum_none"};
monitor::CountRecorder storageUpdateSeqWrite{"storage.chunk_update.seq_write"};

monitor::OperationRecorder storageCommitRecorder{"storage.chunk_commit"};
monitor::CountRecorder storageCommitDirty{"storage.chunk_commit.dirty"};
monitor::CountRecorder storageCommitStale{"storage.chunk_commit.stale"};

}  // namespace

// prepare aio read.
Result<Void> ChunkReplica::aioPrepareRead(ChunkStore &store, AioReadJob &job) {
  auto recordGuard = storageAioReadRecorder.record();

  auto &result = job.result();
  auto &state = job.state();
  const auto &chunkId = job.readIO().key.chunkId;

  // 1. get meta info.
  auto metaResult = store.get(chunkId);
  if (UNLIKELY(metaResult.hasError())) {
    XLOGF(INFO, "{}", metaResult.error());
    RETURN_ERROR(metaResult);
  }
  auto &chunkInfo = (*metaResult)->second;
  const ChunkMetadata &meta = chunkInfo.meta;

  // 2. check meta info.
  result.commitVer = meta.commitVer;
  result.updateVer = meta.updateVer;
  result.commitChainVer = meta.chainVer;
  state.chunkLen = meta.size;
  state.chunkChecksum = meta.checksum();

  if (UNLIKELY(result.commitVer != result.updateVer && !state.readUncommitted)) {
    auto msg = fmt::format("chunk {} {} version mismatch {} != {}", chunkId, meta, result.commitVer, result.updateVer);
    XLOG(ERR, msg);
    storageReadUncommitted.addSample(1);
    return makeError(StorageCode::kChunkNotCommit, std::move(msg));
  }

  // 3. prepare aio read.
  state.readLength = job.alignedLength();
  state.readFd = chunkInfo.view.directFD();
  state.fdIndex = chunkInfo.view.index();
  state.readOffset = meta.innerOffset + job.alignedOffset();

  recordGuard.succ();
  return Void{};
}

// finish aio read.
Result<Void> ChunkReplica::aioFinishRead(ChunkStore &store, AioReadJob &job) {
  auto &readIO = job.readIO();
  auto &result = job.result();
  const auto &chunkId = readIO.key.chunkId;

  // 1. get meta info.
  auto metaResult = store.get(chunkId);
  if (UNLIKELY(!metaResult)) {
    return makeError(std::move(metaResult.error()));
  }
  auto &chunkInfo = (*metaResult)->second;
  const ChunkMetadata &meta = chunkInfo.meta;

  // 2. check meta info.
  if (UNLIKELY(result.updateVer != meta.updateVer)) {
    auto msg = fmt::format("chunk {} {} version outdated {} != {}", chunkId, meta, result.updateVer, meta.updateVer);
    XLOG(ERR, msg);
    storageReadUncommitted.addSample(1);
    return makeError(StorageCode::kChunkNotCommit, std::move(msg));
  }
  return Void{};
}

static Result<uint32_t> doRealWrite(const ChunkId &chunkId,
                                    ChunkInfo &chunkInfo,
                                    const uint8_t *writeData,
                                    uint32_t writeSize,
                                    uint32_t writeOffset) {
#ifndef NDEBUG
  // For debug and unittest.
  static auto flagPath = Path{fmt::format("/tmp/storage_main_write_failed.{}", SysResource::pid())};
  auto checkResult = loadFile(flagPath);
  uint32_t writeErrorPercent = 0;
  if (checkResult && scn::scan(*checkResult, "{}", writeErrorPercent)) {
    if (folly::Random::rand32(100) < writeErrorPercent) {
      auto msg = fmt::format("chunk replica write error for unittest");
      XLOG(ERR, msg);
      return makeError(StorageCode::kChunkWriteFailed, std::move(msg));
    }
  }
#endif

  ChunkMetadata &meta = chunkInfo.meta;
  auto writeResult = chunkInfo.view.write(writeData, writeSize, writeOffset, meta);
  if (LIKELY(bool(writeResult))) {
    meta.size = std::max(uint32_t(meta.size), writeOffset + writeResult.value());
  } else {
    XLOGF(ERR, "chunk replica {} {} write error {}", chunkId, meta, writeResult.error());
  }
  return writeResult;
}

// do write.
Result<uint32_t> ChunkReplica::update(ChunkStore &store, UpdateJob &job, folly::CPUThreadPoolExecutor &executor) {
  auto recordGuard = storageUpdateRecorder.record();

  const auto &writeIO = job.updateIO();
  const auto &options = job.options();
  const auto &chunkId = writeIO.key.chunkId;
  const auto &state = job.state();
  auto &result = job.result();

  if (UNLIKELY(!writeIO.isRemove() &&
               (writeIO.offset >= writeIO.chunkSize || writeIO.offset + writeIO.length > writeIO.chunkSize))) {
    auto msg = fmt::format("chunk {} write offset exceed chunk size {}", chunkId, writeIO);
    XLOG(ERR, msg);
    return makeError(StatusCode::kInvalidArg, std::move(msg));
  }

  // 1. get meta info.
  ChunkInfo chunkInfo;
  bool needCreateChunk = false;
  auto metaResult = store.get(chunkId);
  if (metaResult) {
    chunkInfo = (*metaResult)->second;
  } else if (metaResult.error().code() == StorageCode::kChunkMetadataNotFound) {
    if (writeIO.isRemove()) {
      result.commitVer = result.updateVer = writeIO.updateVer;
      result.commitChainVer = job.commitChainVer();
      return 0;
    } else {
      needCreateChunk = true;
      chunkInfo.meta.chainVer = job.commitChainVer();
      chunkInfo.meta.chunkState = ChunkState::CLEAN;
      chunkInfo.meta.innerFileId.chunkSize = writeIO.chunkSize;
    }
  } else {
    RETURN_AND_LOG_ON_ERROR(metaResult);
  }
  ChunkMetadata &meta = chunkInfo.meta;

  // 2. begin to write.
  auto chunkSize = writeIO.isRemove() ? meta.innerFileId.chunkSize : writeIO.chunkSize;
  result.commitVer = meta.commitVer;
  result.updateVer = meta.updateVer;
  result.checksum = meta.checksum();
  result.commitChainVer = meta.chainVer;
  if (UNLIKELY(meta.innerFileId.chunkSize != chunkSize)) {
    auto msg = fmt::format("chunk {} {} chunk size mismatch {}", chunkId, meta, chunkSize);
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkSizeMismatch, std::move(msg));
  }
  if (UNLIKELY(meta.chunkState == ChunkState::DIRTY && !options.isSyncing)) {
    auto msg = fmt::format("chunk {} {} state not valid", chunkId, meta);
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkNotClean, fmt::format("chunk {} {} state not valid", chunkId, meta));
  }
  if (job.commitChainVer() < meta.chainVer && meta.chunkState == ChunkState::COMMIT) {
    auto msg = fmt::format("chunk {} {} chain version mismatch {} {}", chunkId, meta, writeIO, options);
    reportFatalEvent();
    XLOG(DFATAL, msg);
    return makeError(StorageCode::kChainVersionMismatch, std::move(msg));
  }

  if (writeIO.checksum.type != ChecksumType::NONE && writeIO.length != 0) {
    auto checksum = ChecksumInfo::create(writeIO.checksum.type, state.data, writeIO.length);
    if (checksum != writeIO.checksum) {
      if (!job.requestCtx().debugFlags.faultInjectionEnabled()) {
        reportFatalEvent();
      }
      XLOGF_IF(DFATAL,
               !job.requestCtx().debugFlags.faultInjectionEnabled(),
               "Local checksum {} not equal to checksum {} generated by client, write io: {}",
               checksum,
               writeIO.checksum,
               writeIO);
      return makeError(StorageCode::kChecksumMismatch);
    }
  }

  XLOGF(DBG, "chunk {} {} write begin", chunkId, meta);

  if (options.isSyncing) {
    XLOGF(DBG9, "chunk {} {} sync write: {}", chunkId, meta, writeIO);
    meta.updateVer = writeIO.updateVer;
    meta.commitVer = ChunkVer{writeIO.updateVer - 1};
    meta.recycleState = RecycleState::NORMAL;
  } else if (writeIO.updateVer > 0) {
    if (writeIO.updateVer <= meta.commitVer) {
      auto msg = fmt::format("chunk {} {} committed update {} <= {}", chunkId, meta, writeIO.updateVer, meta.commitVer);
      XLOG(ERR, msg);
      return makeError(StorageCode::kChunkCommittedUpdate, std::move(msg));
    } else if (writeIO.updateVer <= meta.updateVer) {
      auto msg = fmt::format("chunk {} {} stale update {} <= {}", chunkId, meta, writeIO.updateVer, meta.updateVer);
      XLOG(ERR, msg);
      return makeError(StorageCode::kChunkStaleUpdate, std::move(msg));
    } else if (writeIO.updateVer > meta.updateVer + 1) {
      auto msg =
          fmt::format("chunk {} {} missing update {} > {} + 1", chunkId, meta, writeIO.updateVer, meta.updateVer);
      XLOG(ERR, msg);
      return makeError(StorageCode::kChunkMissingUpdate, std::move(msg));
    }
    meta.updateVer = writeIO.updateVer;
  } else {
    meta.updateVer += 1;
    if (meta.updateVer > meta.commitVer + 1) {
      auto msg = fmt::format("chunk {} {} advance update {}", chunkId, meta, writeIO);
      XLOG(ERR, msg);
      return makeError(StorageCode::kChunkAdvanceUpdate, std::move(msg));
    }
  }

  meta.chunkState = ChunkState::DIRTY;
  meta.chainVer = job.commitChainVer();
  meta.lastRequestId = job.requestCtx().tag.requestId;
  meta.lastClientUuid = job.requestCtx().tag.clientId.uuid;
  meta.timestamp = UtcClock::now();
  const bool isAppendWrite = writeIO.offset == meta.size;
  const bool skipPersist = (writeIO.isWrite() && isAppendWrite) || writeIO.isTruncate() || writeIO.isExtend();
  auto setResult = needCreateChunk ? store.createChunk(chunkId, chunkSize, chunkInfo, executor, job.allowToAllocate())
                                   : store.set(chunkId, chunkInfo, !skipPersist);
  if (UNLIKELY(!setResult)) {
    return makeError(std::move(setResult.error()));
  }
  result.commitChainVer = meta.chainVer;
  result.updateVer = meta.updateVer;

  uint32_t chunkSizeBeforeWrite = meta.size;

  // 3. do write operation.
  Result<uint32_t> writeResult = 0;
  if (writeIO.isTruncate() || writeIO.isExtend()) {
    if (writeIO.length <= meta.size) {
      if (writeIO.isTruncate()) {
        writeResult = (meta.size = writeIO.length);
      } else {
        writeResult = meta.size;
      }
    } else {
      // extend the chunk (fill zeros)
      writeResult = doRealWrite(chunkId, chunkInfo, kZeroBytes.data(), writeIO.length - meta.size, meta.size);
      if (writeResult) {
        writeResult = meta.size;  // set result to the actual chunk length if write succeeds
      }
    }
  } else if (writeIO.isRemove()) {
    // remove.
    if (!meta.readyToRemove()) {
      meta.recycleState = RecycleState::REMOVAL_IN_PROGRESS;
    }
    writeResult = 0;
  } else {
    // fill zeros before the write range if there is a gap
    if (meta.size < writeIO.offset) {
      RETURN_AND_LOG_ON_ERROR(chunkInfo.view.write(kZeroBytes.data(), writeIO.offset - meta.size, meta.size, meta));
    }

    // normal write.
    writeResult = doRealWrite(chunkId, chunkInfo, state.data, writeIO.length, writeIO.offset);
    if (writeResult) {
      if (options.isSyncing) meta.size = writeIO.length;
      storageUpdateSeqWrite.addSample(writeIO.offset == chunkSizeBeforeWrite);
    }
  }
  if (UNLIKELY(!writeResult)) {
    return writeResult;  // chunk becomes dirty.
  }

  // update chunk checksum
  auto checksumRes = updateChecksum(chunkInfo, writeIO, chunkSizeBeforeWrite, isAppendWrite);
  if (UNLIKELY(!checksumRes)) {
    return makeError(std::move(checksumRes.error()));
  }

  // 4. finish to write.
  meta.chunkState = ChunkState::CLEAN;

  XLOGF(DBG, "chunk {} {} write finish", chunkId, meta);
  setResult = store.set(chunkId, chunkInfo, !skipPersist);
  if (UNLIKELY(!setResult)) {
    return makeError(std::move(setResult.error()));
  }
  result.checksum = meta.checksum();
  result.commitVer = meta.commitVer;
  result.commitChainVer = meta.chainVer;

  recordGuard.succ();
  return writeResult;
}

Result<Void> ChunkReplica::updateChecksum(ChunkInfo &chunkInfo,
                                          UpdateIO writeIO,
                                          uint32_t chunkSizeBeforeWrite,
                                          bool isAppendWrite) {
  const auto &chunkId = writeIO.key.chunkId;
  ChunkMetadata &meta = chunkInfo.meta;
  auto chunkChecksum = meta.checksum();
  bool combineChecksum = chunkSizeBeforeWrite > 0 && isAppendWrite;

  if (writeIO.isTruncate() || writeIO.isExtend()) {
    writeIO.checksum = ChecksumInfo::create(meta.checksumType, (const uint8_t *)nullptr, 0);
    writeIO.offset = meta.size;
    writeIO.length = 0;
  }

  if (writeIO.checksum.type == ChecksumType::NONE || meta.size == 0) {
    meta.checksumValue = 0;
    storageUpdateChecksumNone.addSample(1);
  } else if (writeIO.offset == 0 && writeIO.length == meta.size) {
    meta.checksumValue = writeIO.checksum.value;
    storageUpdateChecksumReuse.addSample(1);
  } else if (writeIO.checksum.type == chunkChecksum.type && combineChecksum) {
    // combine the chunk checksum and write io checksum if this write appends to existing chunk
    auto combinResult = chunkChecksum.combine(writeIO.checksum, writeIO.length);

    if (UNLIKELY(!combinResult)) {
      XLOGF(ERR,
            "Failed to combine checksums: error {}, chunkId {}, meta {}, write io: {}",
            combinResult.error(),
            chunkId,
            meta,
            writeIO);
      return makeError(combinResult.error());
    }

    meta.checksumValue = chunkChecksum.value;
    storageUpdateChecksumCombine.addSample(1);
  } else {
    // read the prefix of chunk and compute its checksum
    auto prefixChecksum = chunkInfo.view.checksum(writeIO.checksum.type, writeIO.offset, 0, meta);

    if (UNLIKELY(!prefixChecksum)) {
      XLOGF(ERR,
            "Failed to calculate chunk prefix checksum: error {}, chunkId {}, meta {}, write io: {}",
            prefixChecksum.error(),
            chunkId,
            meta,
            writeIO);
      return makeError(std::move(prefixChecksum.error()));
    }

    // read the suffix of chunk and compute its checksum
    uint32_t suffixStart = std::min(writeIO.offset + writeIO.length, meta.size);
    uint32_t suffixLength = meta.size - suffixStart;
    auto suffixChecksum = chunkInfo.view.checksum(writeIO.checksum.type, suffixLength, suffixStart, meta);

    if (UNLIKELY(!suffixChecksum)) {
      XLOGF(ERR,
            "Failed to calculate chunk suffix checksum: error {}, chunkId {}, meta {}, write io: {}",
            suffixChecksum.error(),
            chunkId,
            meta,
            writeIO);
      return makeError(std::move(suffixChecksum.error()));
    }

    prefixChecksum->combine(writeIO.checksum, writeIO.length);
    prefixChecksum->combine(*suffixChecksum, suffixLength);

    meta.checksumValue = prefixChecksum->value;
    storageUpdateChecksumReadChunk.addSample(1);
  }

  meta.checksumType = writeIO.checksum.type;
  return Void{};
}

// commit the version of this chunk.
Result<uint32_t> ChunkReplica::commit(ChunkStore &store, UpdateJob &job) {
  auto recordGuard = storageCommitRecorder.record();

  auto &commitIO = job.commitIO();
  auto &chunkId = commitIO.key.chunkId;
  auto &result = job.result();

  // 1. get meta info.
  auto getResult = store.get(chunkId);
  if (commitIO.isRemove && !getResult && getResult.error().code() == StorageCode::kChunkMetadataNotFound) {
    result.commitVer = result.updateVer = commitIO.commitVer;
    result.commitChainVer = commitIO.commitChainVer;
    return 0;
  }
  RETURN_AND_LOG_ON_ERROR(getResult);

  auto chunkInfo = (*getResult)->second;
  ChunkMetadata &meta = chunkInfo.meta;
  result.commitVer = meta.commitVer;
  result.updateVer = meta.updateVer;
  result.commitChainVer = meta.chainVer;

  if (job.commitChainVer() < meta.chainVer) {
    auto msg = fmt::format("chunk {} {} chain version mismatch {}", chunkId, meta, commitIO);
    reportFatalEvent();
    XLOG(DFATAL, msg);
    return makeError(StorageCode::kChainVersionMismatch, std::move(msg));
  }
  if (commitIO.commitVer > meta.updateVer) {
    auto msg = fmt::format("chunk {} meta {} commit version mismatch", chunkId, meta);
    reportFatalEvent();
    XLOG(DFATAL, msg);
    return makeError(StorageCode::kChunkVersionMismatch, std::move(msg));
  }

  if (commitIO.isForce) {
    meta.chunkState = ChunkState::CLEAN;
    meta.commitVer = commitIO.commitVer;
  } else if (meta.chunkState == ChunkState::DIRTY) {
    auto msg = fmt::format("chunk {} is dirty {}", chunkId, meta);
    XLOG(ERR, msg);
    storageCommitDirty.addSample(1);
    return makeError(StorageCode::kChunkNotClean, std::move(msg));
  } else if (meta.commitVer < commitIO.commitVer) {
    meta.commitVer = commitIO.commitVer;
  } else {
    auto msg = fmt::format("chunk {} stale commit {} > {}", chunkId, meta.commitVer, commitIO.commitVer);
    XLOG(ERR, msg);
    storageCommitStale.addSample(1);
    result.commitVer = meta.commitVer;
    result.commitChainVer = meta.chainVer;
    return makeError(StorageCode::kChunkStaleCommit, std::move(msg));
  }

  if (meta.commitVer == meta.updateVer) {
    meta.chunkState = ChunkState::COMMIT;
    meta.chainVer = job.commitChainVer();
  }
  meta.lastRequestId = job.requestCtx().tag.requestId;
  meta.lastClientUuid = job.requestCtx().tag.clientId.uuid;
  meta.timestamp = UtcClock::now();
  auto metaResult = meta.readyToRemove() ? store.remove(chunkId, chunkInfo) : store.set(chunkId, chunkInfo);
  if (UNLIKELY(!metaResult)) {
    return makeError(std::move(metaResult.error()));
  }
  result.commitVer = meta.commitVer;
  result.commitChainVer = meta.chainVer;

  recordGuard.succ();
  return 0;
}

}  // namespace hf3fs::storage
