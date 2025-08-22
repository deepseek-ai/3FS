#pragma once

#include <folly/experimental/coro/Baton.h>
#include <utility>

#include "chunk_engine/src/cxx.rs.h"
#include "common/net/ib/IBSocket.h"
#include "common/serde/CallContext.h"
#include "common/utils/Duration.h"
#include "fbs/storage/Common.h"
#include "storage/store/ChunkMetadata.h"

namespace hf3fs::storage {

class BatchReadJob;
class StorageTarget;

class ChunkEngineReadJob {
 public:
  ChunkEngineReadJob() = default;
  ChunkEngineReadJob(const ChunkEngineReadJob &) = delete;
  ChunkEngineReadJob(ChunkEngineReadJob &&other)
      : engine_(std::exchange(other.engine_, nullptr)),
        chunk_(std::exchange(other.chunk_, nullptr)) {}

  void set(chunk_engine::Engine *engine, const chunk_engine::Chunk *chunk) {
    reset();
    engine_ = engine;
    chunk_ = chunk;
  }

  void reset() {
    if (engine_ && chunk_) {
      std::exchange(engine_, nullptr)->release_raw_chunk(chunk_);
    }
  }

  auto chunk() const { return chunk_; }

  bool has_chunk() const { return chunk_ != nullptr; }

  ~ChunkEngineReadJob() { reset(); }

 private:
  chunk_engine::Engine *engine_{};
  const chunk_engine::Chunk *chunk_{};
};

class AioReadJob {
 public:
  AioReadJob(const ReadIO &readIO, IOResult &result, BatchReadJob &batch);

  auto &readIO() { return readIO_; }
  auto &result() { return result_; }
  auto &batch() { return batch_; }
  auto &state() { return state_; }

  void setResult(Result<uint32_t> lengthInfo);

  uint32_t alignedOffset() const { return readIO_.offset - state_.headLength; }
  uint32_t alignedLength() const { return readIO_.length + state_.headLength + state_.tailLength; }

  auto startTime() const { return startTime_; }
  void resetStartTime() { startTime_ = RelativeTime::now(); }

 private:
  const ReadIO &readIO_;
  IOResult &result_;
  BatchReadJob &batch_;
  struct State {
    net::RDMABuf localbuf{};
    StorageTarget *storageTarget = nullptr;
    ChunkEngineReadJob chunkEngineJob{};
    SERDE_STRUCT_FIELD(headLength, uint32_t{});
    SERDE_STRUCT_FIELD(tailLength, uint32_t{});
    SERDE_STRUCT_FIELD(readLength, uint32_t{});  // after cropping.
    SERDE_STRUCT_FIELD(readFd, int32_t{});
    SERDE_STRUCT_FIELD(readOffset, uint64_t{});
    SERDE_STRUCT_FIELD(chunkLen, uint32_t{});
    SERDE_STRUCT_FIELD(bufferIndex, uint32_t{});
    SERDE_STRUCT_FIELD(fdIndex, std::optional<uint32_t>{});
    SERDE_STRUCT_FIELD(chunkChecksum, ChecksumInfo{});
    SERDE_STRUCT_FIELD(readUncommitted, false);
  } state_;
  static_assert(serde::Serializable<State>);
  RelativeTime startTime_{};
};

class BatchReadJob {
 public:
  BatchReadJob(std::span<const ReadIO> readIOs, std::span<IOResult> results, ChecksumType checksumType);
  BatchReadJob(const ReadIO &readIO, StorageTarget *target, IOResult &result, ChecksumType checksumType)
      : BatchReadJob(std::span(&readIO, 1), std::span(&result, 1), checksumType) {
    jobs_.back().state().storageTarget = target;
  }
  CoTask<void> complete() { co_await baton_; }
  size_t addBufferToBatch(serde::CallContext::RDMATransmission &batch);
  size_t copyToRespBuffer(std::vector<uint8_t> &buffer);
  void finish(AioReadJob *job);
  auto checksumType() const { return checksumType_; }
  bool recalculateChecksum() const { return recalculateChecksum_; }
  void setRecalculateChecksum(bool value = true) { recalculateChecksum_ = value; }
  auto &front() { return jobs_.front(); }
  auto &front() const { return jobs_.front(); }
  auto startTime() const { return startTime_.load(); }
  void resetStartTime() { startTime_ = RelativeTime::now(); }

 private:
  friend class AioReadJobIterator;
  std::vector<AioReadJob> jobs_;
  folly::coro::Baton baton_;
  std::atomic<uint64_t> finishedCount_{};
  std::atomic<RelativeTime> startTime_ = RelativeTime::now();
  const ChecksumType checksumType_;
  bool recalculateChecksum_ = false;
};

class AioReadJobIterator {
 public:
  AioReadJobIterator() = default;
  AioReadJobIterator(BatchReadJob *batch)
      : batch_(batch),
        end_(batch->jobs_.size()) {}
  AioReadJobIterator(BatchReadJob *batch, uint32_t start, uint32_t size)
      : batch_(batch),
        begin_(start),
        end_(std::min((uint32_t)batch->jobs_.size(), start + size)) {}

  operator bool() const { return begin_ < end_; }
  bool isNull() const { return batch_ == nullptr; }
  AioReadJob &operator*() { return batch_->jobs_[begin_]; }
  AioReadJob *operator->() { return &batch_->jobs_[begin_]; }
  AioReadJob *operator++(int) { return &batch_->jobs_[begin_++]; }

  auto startTime() const { return startTime_; }
  auto resetStartTime() { startTime_ = RelativeTime::now(); }

 private:
  BatchReadJob *batch_ = nullptr;
  uint32_t begin_ = 0;
  uint32_t end_ = 0;
  RelativeTime startTime_ = RelativeTime::now();
};

}  // namespace hf3fs::storage
