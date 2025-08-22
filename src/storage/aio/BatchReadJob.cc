#include "storage/aio/BatchReadJob.h"

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "storage/store/StorageTarget.h"

namespace hf3fs::storage {

monitor::CountRecorder rdmaWriteCount{"storage.rdma_write.count"};
monitor::CountRecorder rdmaWriteFails{"storage.rdma_write.fails"};
monitor::CountRecorder rdmaWriteBytes{"storage.rdma_write.bytes"};
monitor::LatencyRecorder batchReadLatency{"storage.aio.batch_latency"};

monitor::CountRecorder aioChecksumMismatch{"storage.aio.checksum_mismatch"};

AioReadJob::AioReadJob(const ReadIO &readIO, IOResult &result, BatchReadJob &batch)
    : readIO_(readIO),
      result_(result),
      batch_(batch) {
  state_.headLength = readIO_.offset % kAIOAlignSize;
  state_.tailLength = (kAIOAlignSize - (readIO_.offset + readIO_.length) % kAIOAlignSize) % kAIOAlignSize;
}

void AioReadJob::setResult(Result<uint32_t> lengthInfo) {
  if (lengthInfo) {
    auto checksumType = batch_.checksumType();

    if (checksumType == ChecksumType::NONE) {
      result_.checksum = {ChecksumType::NONE, 0U};  // do not return checksum
    } else if (checksumType == state_.chunkChecksum.type && readIO_.offset == 0 && *lengthInfo == state_.chunkLen) {
      result_.checksum = state_.chunkChecksum;  // use chunk checksum if the full chunk is read
    } else {                                    // calculate checksum of the read data
      auto dataBuf = state_.localbuf.subrange(state_.headLength, *lengthInfo);
      result_.checksum = ChecksumInfo::create(checksumType, dataBuf.ptr(), dataBuf.size());
    }

    // check chunk version.
    auto result = state_.storageTarget->aioFinishRead(*this);
    if (UNLIKELY(!result)) {
      lengthInfo = makeError(std::move(result.error()));
    }

    if (batch_.recalculateChecksum() && readIO_.offset == 0 && *lengthInfo == state_.chunkLen) {
      auto realChecksum = ChecksumInfo::create(state_.chunkChecksum.type, state_.localbuf.ptr(), *lengthInfo);
      if (UNLIKELY(realChecksum != state_.chunkChecksum)) {
        aioChecksumMismatch.addSample(1);
        auto msg = fmt::format("aio checksum mismatch, read: {}, state: {}, checksum: {}",
                               readIO(),
                               state(),
                               realChecksum.value);
        XLOG(CRITICAL, msg);
        lengthInfo = makeError(StorageCode::kChecksumMismatch, std::move(msg));
      }
    }
  }

  XLOGF_IF(WARN, !lengthInfo, "Read job failed, result: {}, read io: {}, state: {}", lengthInfo, readIO_, state_);
  XLOGF(DBG7, "Read job completed, result: {}, read io: {}, state: {}", lengthInfo, readIO_, state_);

  result_.lengthInfo = std::move(lengthInfo);
  state_.chunkEngineJob.reset();
  batch_.finish(this);
}

BatchReadJob::BatchReadJob(std::span<const ReadIO> readIOs, std::span<IOResult> results, ChecksumType checksumType)
    : checksumType_(checksumType) {
  auto batchSize = readIOs.size();
  jobs_.reserve(batchSize);
  for (auto i = 0ul; i < batchSize; ++i) {
    jobs_.emplace_back(readIOs[i], results[i], *this);
  }
}

size_t BatchReadJob::addBufferToBatch(serde::CallContext::RDMATransmission &batch) {
  size_t writeCount = 0;
  size_t writeBytes = 0;
  for (auto &job : jobs_) {
    if (job.result().lengthInfo) {
      auto length = *job.result().lengthInfo;
      auto localbuf = job.state().localbuf.subrange(job.state().headLength, length);
      auto result = batch.add(job.readIO().rdmabuf, localbuf);
      if (UNLIKELY(!result)) {
        rdmaWriteFails.addSample(1);
        job.result().lengthInfo = makeError(std::move(result.error()));
      } else {
        ++writeCount;
        writeBytes += length;
      }
    }
  }
  rdmaWriteCount.addSample(writeCount);
  rdmaWriteBytes.addSample(writeBytes);
  return writeBytes;
}

size_t BatchReadJob::copyToRespBuffer(std::vector<uint8_t> &buffer) {
  size_t sendBytes = 0;
  for (auto &job : jobs_) {
    if (job.result().lengthInfo) {
      // check chunk version.
      auto length = *job.result().lengthInfo;
      auto localbuf = job.state().localbuf.subrange(job.state().headLength, length);
      size_t bufEnd = buffer.size();

      if (buffer.empty()) buffer.reserve(localbuf.size() * jobs_.size());
      buffer.resize(buffer.size() + localbuf.size());
      std::memcpy(&buffer[bufEnd], localbuf.ptr(), localbuf.size());

      sendBytes += length;
    }
  }
  return sendBytes;
}

void BatchReadJob::finish(AioReadJob *job) {
  (void)job;
  if (++finishedCount_ == jobs_.size()) {
    batchReadLatency.addSample(RelativeTime::now() - startTime());
    baton_.post();
  }
}

}  // namespace hf3fs::storage
