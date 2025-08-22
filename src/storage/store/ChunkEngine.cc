#include "ChunkEngine.h"

#include "chunk_engine/src/cxx.rs.h"
#include "fbs/storage/Common.h"
#include "storage/update/UpdateJob.h"

namespace hf3fs::storage {
namespace {

monitor::OperationRecorder storageUpdateRecorder{"storage.engine_update"};
monitor::OperationRecorder storageCommitRecorder{"storage.engine_commit"};

}  // namespace

Result<uint32_t> ChunkEngine::update(chunk_engine::Engine &engine, UpdateJob &job) {
  auto recordGuard = storageUpdateRecorder.record();

  // 1. prepare.
  const auto &updateIO = job.updateIO();
  const auto &chunkId = updateIO.key.chunkId;
  const auto &options = job.options();
  const auto &state = job.state();
  auto &result = job.result();

  auto chainId = updateIO.key.vChainId.chainId;
  std::string key;
  key.reserve(sizeof(chainId) + chunkId.data().size());
  key.append((const char *)&chainId, sizeof(chainId));
  key.append(chunkId.data());

  // 2. start update.
  chunk_engine::UpdateReq req{};
  if (updateIO.isTruncate()) {
    req.is_truncate = true;
  } else if (updateIO.isRemove()) {
    req.is_remove = true;
  }
  req.is_syncing = options.isSyncing;
  req.update_ver = updateIO.updateVer;
  req.chain_ver = job.commitChainVer();
  if (updateIO.checksum.type == ChecksumType::CRC32C) {
    req.checksum = ~updateIO.checksum.value;
  } else if (state.data) {
    req.without_checksum = true;
  }
  if (updateIO.isWrite()) {
    req.length = updateIO.length;
    req.offset = updateIO.offset;
  } else {
    req.length = 0;
    req.offset = updateIO.length;
  }
  req.data = reinterpret_cast<uint64_t>(state.data);
  req.last_request_id = job.requestCtx().tag.requestId;
  auto clientId = job.requestCtx().tag.clientId.uuid.asStringView();
  req.last_client_low = *(const uint64_t *)clientId.data();
  req.last_client_high = *(const uint64_t *)(clientId.data() + 8);

  std::string error{};
  auto chunk = engine.update_raw_chunk(toSlice(key), req, error);
  result.updateVer = result.commitVer = ChunkVer{req.out_commit_ver};
  result.commitChainVer = ChainVer{req.out_chain_ver};
  if (req.is_remove && req.out_non_existent) {
    result.checksum = ChecksumInfo{ChecksumType::NONE, 0};
  } else {
    result.checksum = ChecksumInfo{ChecksumType::CRC32C, ~req.out_checksum};
  }

  if (UNLIKELY(!error.empty())) {
    return makeError(req.out_error_code, std::move(error));
  }

  job.chunkEngineJob().set(engine, chunk);

  recordGuard.succ();
  if (updateIO.isTruncate() || updateIO.isExtend()) {
    return chunk->raw_meta().len;
  }
  return updateIO.length;
}

Result<uint32_t> ChunkEngine::commit(chunk_engine::Engine &engine, UpdateJob &job, bool sync) {
  auto recordGuard = storageCommitRecorder.record();

  const auto &commitIO = job.commitIO();
  const auto &chunkId = commitIO.key.chunkId;
  auto &result = job.result();

  auto chainId = commitIO.key.vChainId.chainId;
  std::string key;
  key.reserve(sizeof(chainId) + chunkId.data().size());
  key.append((const char *)&chainId, sizeof(chainId));
  key.append(chunkId.data());

  auto chunk = job.chunkEngineJob().chunk();
  chunk->set_chain_ver(job.commitChainVer());
  auto &meta = chunk->raw_meta();
  result.updateVer = result.commitVer = ChunkVer{meta.chunk_ver};
  result.commitChainVer = ChainVer{meta.chain_ver};

  std::string error;
  engine.commit_raw_chunk(chunk, sync, error);
  job.chunkEngineJob().release();
  if (UNLIKELY(!error.empty())) {
    return makeError(StorageCode::kChunkMetadataSetError, std::move(error));
  }

  recordGuard.succ();
  return uint32_t{};
}

}  // namespace hf3fs::storage
