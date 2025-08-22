#pragma once

#include <folly/experimental/coro/Baton.h>

#include "chunk_engine/src/cxx.rs.h"
#include "fbs/storage/Common.h"
#include "storage/store/ChunkMetadata.h"
#include "storage/store/ChunkStore.h"

namespace hf3fs::storage {

class StorageTarget;

class ChunkEngineUpdateJob {
 public:
  ChunkEngineUpdateJob() = default;
  ChunkEngineUpdateJob(const ChunkEngineUpdateJob &) = delete;
  ChunkEngineUpdateJob(ChunkEngineUpdateJob &&other)
      : engine_(std::exchange(other.engine_, nullptr)),
        chunk_(std::exchange(other.chunk_, nullptr)) {}

  void set(chunk_engine::Engine &engine, chunk_engine::WritingChunk *chunk) {
    reset();
    engine_ = &engine;
    chunk_ = chunk;
  }

  auto release() { return std::exchange(engine_, nullptr); }
  auto chunk() const { return chunk_; }

  void reset() {
    if (engine_ && chunk_) {
      release()->release_writing_chunk(chunk_);
    }
  }

  ~ChunkEngineUpdateJob() { reset(); }

 private:
  chunk_engine::Engine *engine_{};
  chunk_engine::WritingChunk *chunk_{};
};

class UpdateJob {
 public:
  UpdateJob(ServiceRequestContext &requestCtx,
            const UpdateIO &updateIO,
            const UpdateOptions &options,
            ChunkEngineUpdateJob &chunkEngineJob,
            std::shared_ptr<StorageTarget> target,
            bool allowToAllocate = true)
      : requestCtx_(requestCtx),
        type_(updateIO.updateType),
        chunkId_(updateIO.key.chunkId),
        target_(std::move(target)),
        updateIO_(updateIO),
        chunkEngineJob_(chunkEngineJob),
        options_(options),
        allowToAllocate_(allowToAllocate) {}

  UpdateJob(ServiceRequestContext &requestCtx,
            const CommitIO &commitIO,
            const UpdateOptions &options,
            ChunkEngineUpdateJob &chunkEngineJob,
            std::shared_ptr<StorageTarget> target)
      : requestCtx_(requestCtx),
        type_(UpdateType::COMMIT),
        chunkId_(commitIO.key.chunkId),
        target_(std::move(target)),
        commitIO_(commitIO),
        chunkEngineJob_(chunkEngineJob),
        options_(options) {}

  auto &requestCtx() { return requestCtx_; }
  auto type() const { return type_; }
  const auto &chunkId() const { return chunkId_; }
  auto &target() const { return target_; }
  auto &updateIO() { return updateIO_; }
  auto &commitIO() { return commitIO_; }
  auto &chunkEngineJob() { return chunkEngineJob_; }
  auto &options() { return options_; }
  auto &result() { return result_; }
  auto &state() { return state_; }
  auto allowToAllocate() const { return allowToAllocate_; }
  ChainVer commitChainVer() const {
    if (options_.isSyncing) {
      return options_.commitChainVer;
    } else if (type() == UpdateType::COMMIT) {
      return commitIO_.commitChainVer;
    } else {
      return updateIO_.key.vChainId.chainVer;
    }
  }

  CoTask<void> complete() const { co_await baton_; }
  void setResult(Result<uint32_t> result) {
    result_.lengthInfo = std::move(result);
    baton_.post();
  }

 protected:
  ServiceRequestContext &requestCtx_;
  UpdateType type_;
  ChunkId chunkId_;
  std::shared_ptr<StorageTarget> target_;
  UpdateIO updateIO_;
  CommitIO commitIO_;
  ChunkEngineUpdateJob &chunkEngineJob_;
  UpdateOptions options_;
  IOResult result_;
  folly::coro::Baton baton_;
  struct State {
    const uint8_t *data = nullptr;
  } state_;
  bool allowToAllocate_ = true;
};

}  // namespace hf3fs::storage
