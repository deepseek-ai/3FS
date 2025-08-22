#pragma once

#include <limits>

#include "chunk_engine/src/cxx.rs.h"
#include "common/utils/UtcTime.h"
#include "fbs/storage/Common.h"
#include "storage/aio/BatchReadJob.h"
#include "storage/update/UpdateJob.h"

namespace hf3fs::storage {

struct ChunkEngine {
  static void copyMeta(const chunk_engine::RawMeta &in, ChunkMetadata &out) {
    out.commitVer = ChunkVer{in.chunk_ver};
    out.updateVer = ChunkVer{in.chunk_ver};
    out.chainVer = ChainVer{in.chain_ver};
    out.size = in.len;
    out.chunkState = ChunkState::COMMIT;
    out.recycleState = RecycleState::NORMAL;
    out.checksumType = ChecksumType::CRC32C;
    out.checksumValue = ~in.checksum;
    out.innerFileId = ChunkFileId{std::max(uint32_t(in.pos >> 48 << 16), 512u * 1024), 256};
    out.innerOffset = in.pos;
    out.timestamp = UtcTime::fromMicroseconds(in.timestamp);
    out.lastRequestId = RequestId{in.last_request_id};
    out.lastClientUuid = Uuid::from(in.last_client_low, in.last_client_high);
  }

  static rust::Slice<const uint8_t> toSlice(const std::string &key) {
    return rust::Slice<const uint8_t>{(const uint8_t *)key.data(), key.size()};
  }

  static Result<Void> aioPrepareRead(chunk_engine::Engine &engine, AioReadJob &job) {
    auto &state = job.state();

    if (!state.chunkEngineJob.has_chunk()) {
      const auto &chunkId = job.readIO().key.chunkId;
      auto chainId = job.readIO().key.vChainId.chainId;

      std::string key;
      key.reserve(sizeof(chainId) + chunkId.data().size());
      key.append((const char *)&chainId, sizeof(chainId));
      key.append(chunkId.data());

      std::string error;
      auto chunk = engine.get_raw_chunk(toSlice(key), error);
      if (UNLIKELY(!error.empty())) {
        return makeError(StorageCode::kChunkMetadataGetError, std::move(error));
      }

      if (chunk == nullptr) {
        return makeError(StorageCode::kChunkMetadataNotFound);
      }

      state.chunkEngineJob.set(&engine, chunk);
    }

    auto &result = job.result();
    auto &meta = state.chunkEngineJob.chunk()->raw_meta();
    result.commitVer = ChunkVer{meta.chunk_ver};
    result.updateVer = ChunkVer{meta.chunk_ver};
    result.commitChainVer = ChainVer{meta.chain_ver};
    state.chunkLen = meta.len;
    state.chunkChecksum = ChecksumInfo{ChecksumType::CRC32C, ~meta.checksum};

    auto chunkInfo = state.chunkEngineJob.chunk()->fd_and_offset();
    state.readLength = job.alignedLength();
    state.readFd = chunkInfo.fd;
    state.readOffset = chunkInfo.offset + job.alignedOffset();

    return Void{};
  }

  static Result<uint32_t> update(chunk_engine::Engine &engine, UpdateJob &job);

  static Result<uint32_t> commit(chunk_engine::Engine &engine, UpdateJob &job, bool sync);

  static Result<ChunkMetadata> queryChunk(chunk_engine::Engine &engine, const ChunkId &chunkId, ChainId chainId) {
    std::string key;
    key.reserve(sizeof(chainId) + chunkId.data().size());
    key.append((const char *)&chainId, sizeof(chainId));
    key.append(chunkId.data());

    std::string error;
    auto chunk = engine.get_raw_chunk(toSlice(key), error);
    if (UNLIKELY(!error.empty())) {
      return makeError(StorageCode::kChunkMetadataGetError, std::move(error));
    }

    if (chunk == nullptr) {
      return makeError(StorageCode::kChunkMetadataNotFound);
    }

    ChunkMetadata out;
    copyMeta(chunk->raw_meta(), out);

    engine.release_raw_chunk(chunk);
    return out;
  }

  static Result<std::vector<std::pair<ChunkId, ChunkMetadata>>> queryChunks(chunk_engine::Engine &engine,
                                                                            const ChunkIdRange &chunkIdRange,
                                                                            ChainId chainId) {
    const auto &beginChunkId = chunkIdRange.begin;
    std::string beginKey;
    beginKey.reserve(sizeof(chainId) + beginChunkId.data().size());
    beginKey.append((const char *)&chainId, sizeof(chainId));
    beginKey.append(beginChunkId.data());

    const auto &endChunkId = chunkIdRange.end;
    std::string endKey;
    endKey.reserve(sizeof(chainId) + endChunkId.data().size());
    endKey.append((const char *)&chainId, sizeof(chainId));
    endKey.append(endChunkId.data());

    std::string error;
    auto chunks =
        engine.query_raw_chunks(toSlice(beginKey), toSlice(endKey), chunkIdRange.maxNumChunkIdsToProcess, error);
    if (UNLIKELY(!error.empty())) {
      return makeError(StorageCode::kChunkMetadataGetError, std::move(error));
    }

    auto len = chunks->len();
    std::vector<std::pair<ChunkId, ChunkMetadata>> out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
      auto chunkId = chunks->chunk_id(i);
      auto &in = chunks->chunk_meta(i);
      out.emplace_back();
      out.back().first =
          ChunkId(std::string_view{(const char *)chunkId.data() + sizeof(chainId), chunkId.length() - sizeof(chainId)});
      copyMeta(in, out.back().second);
    }
    return out;
  }

  static Result<std::vector<ChunkId>> queryUncommittedChunks(chunk_engine::Engine &engine, ChainId chainId) {
    rust::Slice<const uint8_t> prefix{(const uint8_t *)&chainId, sizeof(chainId)};

    std::string error;
    auto chunks = engine.query_uncommitted_raw_chunks(prefix, error);
    if (UNLIKELY(!error.empty())) {
      return makeError(StorageCode::kChunkMetadataGetError, std::move(error));
    }

    auto len = chunks->len();
    std::vector<ChunkId> out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
      auto chunkId = chunks->chunk_id(i);
      out.push_back(ChunkId(
          std::string_view{(const char *)chunkId.data() + sizeof(chainId), chunkId.length() - sizeof(chainId)}));
    }
    return out;
  }

  static Result<Void> resetUncommittedChunks(chunk_engine::Engine &engine, ChainId chainId, ChainVer chainVer) {
    rust::Slice<const uint8_t> prefix{(const uint8_t *)&chainId, sizeof(chainId)};

    std::string error;
    auto chunks = engine.handle_uncommitted_raw_chunks(prefix, chainVer, error);
    if (UNLIKELY(!error.empty())) {
      XLOGF(CRITICAL, "reset uncommitted chunks failed: {}, chain {}", error, chainId);
      return makeError(StorageCode::kChunkMetadataGetError, std::move(error));
    }

    auto len = chunks->len();
    XLOGF_IF(CRITICAL, len > 0, "reset uncommitted chunks succ, chain: {}, size: {}", chainId, len);
    for (size_t i = 0; i < len; ++i) {
      auto chunkId = chunks->chunk_id(i);
      auto id =
          ChunkId(std::string_view{(const char *)chunkId.data() + sizeof(chainId), chunkId.length() - sizeof(chainId)});
      auto &in = chunks->chunk_meta(i);
      ChunkMetadata meta{};
      copyMeta(in, meta);
      XLOGF(CRITICAL, "reset uncommitted chain {} chunk {} meta {}", chainId, id, meta);
    }

    return Void{};
  }

  static Result<Void> removeAllChunks(chunk_engine::Engine &engine, ChainId chainId) {
    std::string key;
    key.reserve(sizeof(chainId));
    key.append((const char *)&chainId, sizeof(chainId));

    std::string error;
    engine.raw_batch_remove(toSlice(key), toSlice(key), std::numeric_limits<uint64_t>::max(), error);
    if (UNLIKELY(!error.empty())) {
      return makeError(StorageCode::kChunkMetadataSetError, std::move(error));
    }
    return Void{};
  }

  static Result<Void> getAllMetadata(chunk_engine::Engine &engine, ChainId chainId, ChunkMetaVector &metadataVec) {
    rust::Slice<const uint8_t> prefix{(const uint8_t *)&chainId, sizeof(chainId)};
    std::string error;
    auto chunks = engine.query_all_raw_chunks(prefix, error);
    if (UNLIKELY(!error.empty())) {
      return makeError(StorageCode::kChunkMetadataGetError, std::move(error));
    }

    auto len = chunks->len();
    metadataVec.reserve(len);
    for (size_t i = 0; i < len; ++i) {
      auto chunkId = chunks->chunk_id(i);
      auto &in = chunks->chunk_meta(i);

      metadataVec.emplace_back();
      auto &out = metadataVec.back();

      out.chunkId =
          ChunkId(std::string_view{(const char *)chunkId.data() + sizeof(chainId), chunkId.length() - sizeof(chainId)});
      out.updateVer = ChunkVer{in.chunk_ver};
      out.commitVer = ChunkVer{in.chunk_ver};
      out.chainVer = ChainVer{in.chain_ver};
      out.chunkState = ChunkState::COMMIT;
      out.checksum = ChecksumInfo{ChecksumType::CRC32C, ~in.checksum};
      out.length = in.len;
      if (chunks->chunk_uncommitted(i)) {
        out.commitVer = ChunkVer{out.commitVer - 1};
        out.chunkState = ChunkState::CLEAN;
      }
    }
    std::sort(metadataVec.begin(), metadataVec.end(), [](auto &a, auto &b) { return a.chunkId > b.chunkId; });
    return Void{};
  }

  static Result<Void> getAllMetadataMap(chunk_engine::Engine &engine,
                                        std::unordered_map<ChunkId, ChunkMetadata> &metas,
                                        ChainId chainId) {
    rust::Slice<const uint8_t> prefix{(const uint8_t *)&chainId, sizeof(chainId)};
    std::string error;
    auto chunks = engine.query_all_raw_chunks(prefix, error);
    if (UNLIKELY(!error.empty())) {
      return makeError(StorageCode::kChunkMetadataGetError, std::move(error));
    }

    auto len = chunks->len();
    metas.reserve(metas.size() + len);
    for (size_t i = 0; i < len; ++i) {
      auto chunkId = chunks->chunk_id(i);
      auto &meta = metas[ChunkId(
          std::string_view{(const char *)chunkId.data() + sizeof(chainId), chunkId.length() - sizeof(chainId)})];
      copyMeta(chunks->chunk_meta(i), meta);
      if (chunks->chunk_uncommitted(i)) {
        meta.commitVer = ChunkVer{meta.commitVer - 1};
        meta.chunkState = ChunkState::CLEAN;
      }
    }

    return Void{};
  }

  static uint64_t chainUsedSize(chunk_engine::Engine &engine, ChainId chainId) {
    rust::Slice<const uint8_t> slice{(const uint8_t *)&chainId, sizeof(chainId)};
    std::string error;
    auto size = engine.query_raw_used_size(slice, error);
    if (UNLIKELY(!error.empty())) {
      XLOGF(ERR, "query chunk engine chain used size error: chain {} error {}", chainId, error);
    }
    return size;
  }
};

}  // namespace hf3fs::storage
