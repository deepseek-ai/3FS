#include "storage/store/ChunkFileView.h"

#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>
#include <thread>
#include <utility>

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "common/utils/ExponentialBackoffRetry.h"
#include "storage/store/ChunkMetadata.h"

namespace hf3fs::storage {

monitor::OperationRecorder storageReadRecord{"storage.pread"};
monitor::OperationRecorder storageWriteRecord{"storage.pwrite"};
monitor::DistributionRecorder storageWriteSize{"storage.pwrite.size"};
monitor::CountRecorder storageWriteDirect{"storage.pwrite.direct"};

Result<uint32_t> ChunkFileView::read(uint8_t *buf, size_t size, size_t offset, bool direct /* = false */) const {
  auto recordGuard = storageReadRecord.record();

  int fd = direct ? direct_ : normal_;
  uint32_t r = 0;
  while (size > 0) {
    int ret = ::pread(fd, buf, size, offset);
    if (LIKELY(ret > 0)) {
      r += ret;
      buf += ret;
      size -= ret;
      offset += ret;
    } else if (ret == 0) {
      break;
    } else {
      auto msg = fmt::format("read chunk file failed: fd {}, offset {}, errno {}", fd, offset, errno);
      XLOG(ERR, msg);
      return makeError(StorageCode::kChunkReadFailed, std::move(msg));
    }
  }

  recordGuard.succ();
  return r;
}

Result<uint32_t> ChunkFileView::write(const uint8_t *buf, size_t size, size_t offset, const ChunkMetadata &meta) {
  auto recordGuard = storageWriteRecord.record();
  storageWriteSize.addSample(size);
  if (UNLIKELY(size + offset > meta.innerFileId.chunkSize)) {
    auto msg = fmt::format("chunk write exceed chunk size, meta {}, size {}, offset {}", meta, size, offset);
    reportFatalEvent();
    XLOG(DFATAL, msg);
    return makeError(StatusCode::kInvalidArg, std::move(msg));
  }
  offset += meta.innerOffset;
  int fd = normal_;
  if (size % kAIOAlignSize == 0 && offset % kAIOAlignSize == 0 &&
      reinterpret_cast<uint64_t>(buf) % kAIOAlignSize == 0) {
    fd = direct_;
    storageWriteDirect.addSample(1);
    XLOGF(DBG, "use direct fd for write: fd {}, size {}, offset {}", fd, size, offset);
  }
  uint32_t w = 0;
  ExponentialBackoffRetry retry(100_ms, 5_s, 30_s);
  while (size > 0) {
    int ret = ::pwrite(fd, buf, size, offset);
    if (LIKELY(ret > 0)) {
      w += ret;
      buf += ret;
      size -= ret;
      offset += ret;
    } else {
      auto msg = fmt::format("write chunk file failed: fd {}, direct {}, buf {}, offset {}, size {}, ret {}, errno {}",
                             fd,
                             fd == direct_,
                             fmt::ptr(buf),
                             offset,
                             size,
                             ret,
                             errno);
      XLOG(ERR, msg);
      auto waitTime = retry.getWaitTime();
      if (waitTime.count() == 0) {
        return makeError(StorageCode::kChunkWriteFailed, std::move(msg));
      }
      std::this_thread::sleep_for(waitTime);
    }
  }
  recordGuard.succ();
  return w;
}

Result<ChecksumInfo> ChunkFileView::checksum(ChecksumType type, size_t size, size_t offset, const ChunkMetadata &meta) {
  if (UNLIKELY(size + offset > meta.innerFileId.chunkSize)) {
    auto msg = fmt::format("chunk write exceed chunk size, meta {}, size {}, offset {}", meta, size, offset);
    reportFatalEvent();
    XLOG(DFATAL, msg);
    return makeError(StatusCode::kInvalidArg, std::move(msg));
  }
  offset += meta.innerOffset;
  ChunkDataIterator iter(*this, size, offset);
  auto checksum = ChecksumInfo::create(type, &iter, size);
  if (checksum.type == ChecksumType::NONE) return makeError(StorageCode::kChunkReadFailed);
  return checksum;
}

std::pair<const uint8_t *, size_t> ChunkDataIterator::next() {
  if (length_ == 0) return {nullptr, 0};

  size_t readSize = std::min(length_, ChecksumInfo::kChunkSize);
  bool directIO = readSize % kAIOAlignSize == 0 && offset_ % kAIOAlignSize == 0;
  auto readRes = chunkFile_.read(data_, readSize, offset_, directIO);

  if (!readRes) {
    XLOGF(ERR, "Cannot calculate checksum since read failed, error: {}", readRes);
    return {nullptr, 0};
  } else if (*readRes != readSize) {
    XLOGF(ERR, "Cannot calculate checksum since read size {} not equal to requested size {}", *readRes, readSize);
    return {nullptr, 0};
  }

  offset_ += readSize;
  length_ -= readSize;

  return {data_, readSize};
}

}  // namespace hf3fs::storage
