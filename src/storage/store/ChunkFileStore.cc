#include "storage/store/ChunkFileStore.h"

#include <fcntl.h>
#include <folly/logging/xlog.h>
#include <limits>

#include "common/monitor/Recorder.h"
#include "common/utils/Result.h"
#include "storage/store/ChunkMetadata.h"

namespace hf3fs::storage {
namespace {

monitor::OperationRecorder punchHoleRecorder{"storage.punch_hole"};
monitor::OperationRecorder allocateSpaceRecorder{"storage.allocate_space"};

}  // namespace

Result<Void> ChunkFileStore::create(const PhysicalConfig &config) {
  path_ = config.path;
  physicalFileCount_ = config.physical_file_count;
  for (auto &chunkSize : config.chunk_size_list) {
    RETURN_AND_LOG_ON_ERROR(createInnerFile(chunkSize));
  }
  return Void{};
}

Result<Void> ChunkFileStore::load(const PhysicalConfig &config) {
  path_ = config.path;
  physicalFileCount_ = config.physical_file_count;
  for (uint32_t chunkSize : config.chunk_size_list) {
    if (config_.preopen_chunk_size_list().contains(chunkSize)) {
      for (auto i = 0u; i < physicalFileCount_; ++i) {
        RETURN_AND_LOG_ON_ERROR(openInnerFile(ChunkFileId{chunkSize, i}));
      }
    }
  }
  return Void{};
}

Result<Void> ChunkFileStore::addChunkSize(const std::vector<Size> &sizeList) {
  for (auto &chunkSize : sizeList) {
    RETURN_AND_LOG_ON_ERROR(createInnerFile(chunkSize));
    XLOGF(WARNING, "chunk inner files are created, path {}, size {}", path_, chunkSize);
  }
  return Void{};
}

Result<ChunkFileView> ChunkFileStore::open(ChunkFileId fileId) {
  auto openResult = openInnerFile(fileId);
  if (UNLIKELY(!openResult)) {
    XLOGF(ERR, "open file {} failed: {}", fileId, openResult.error());
    return makeError(std::move(openResult.error()));
  }
  auto &innerFile = **openResult;

  ChunkFileView file;
  file.normal_ = innerFile.normal_;
  file.direct_ = innerFile.direct_;
  file.index_ = innerFile.index_;
  return file;
}

Result<Void> ChunkFileStore::punchHole(ChunkFileId fileId, size_t offset) {
  auto recordGuard = punchHoleRecorder.record();

  auto openResult = openInnerFile(fileId);
  if (UNLIKELY(!openResult)) {
    XLOGF(ERR, "open file {} failed: {}", fileId, openResult.error());
    return makeError(std::move(openResult.error()));
  }
  auto &innerFile = **openResult;

  int ret = ::fallocate(innerFile.direct_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, fileId.chunkSize);
  XLOGF(DBG, "punch hole {}, offset {}", fileId, Size::toString(offset));
  if (UNLIKELY(ret == -1)) {
    XLOGF(ERR, "punch hole to {} failed: {}", fileId, errno);
    return makeError(StorageCode::kPunchHoleFailed, fmt::format("punch hole to {} failed: {}", fileId, errno));
  }

  recordGuard.succ();
  return Void{};
}

Result<Void> ChunkFileStore::allocate(ChunkFileId fileId, size_t offset, size_t size) {
  auto recordGuard = allocateSpaceRecorder.record();

  auto openResult = openInnerFile(fileId);
  RETURN_AND_LOG_ON_ERROR(openResult);
  auto &innerFile = **openResult;

  int ret = ::fallocate(innerFile.direct_, 0, offset, size);
  XLOGF(DBG, "allocate {}, offset {}, size {}", fileId, Size::toString(offset), Size::toString(size));
  if (UNLIKELY(ret == -1)) {
    auto msg = fmt::format("allocate to {} failed: {}", fileId, errno);
    XLOG(ERR, msg);
    return makeError(StorageCode::kPunchHoleFailed, std::move(msg));
  }

  recordGuard.succ();
  return Void{};
}

Result<FileDescriptor *> ChunkFileStore::openInnerFile(ChunkFileId fileId, bool createFile /* = false */) {
  // 1. try to find in TLS cache.
  auto &cache = (*tlsCache_)[fileId];
  if (LIKELY(cache != nullptr)) {
    return cache;
  }

  Path filePath = path_ / Size::toString(fileId.chunkSize) / fmt::format("{:02X}", fileId.chunkIdx);
  auto openResult = globalFileStore_.open(filePath, createFile);
  RETURN_AND_LOG_ON_ERROR(openResult);
  cache = *openResult;
  return openResult;
}

Result<Void> ChunkFileStore::createInnerFile(Size chunkSize) {
  if (chunkSize < kAIOAlignSize) {
    auto msg = fmt::format("chunk size too small: {}", chunkSize);
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkStoreInitFailed, std::move(msg));
  }
  if (chunkSize % kAIOAlignSize) {
    auto msg = fmt::format("chunk size not aligned: {}", chunkSize);
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkStoreInitFailed, std::move(msg));
  }
  if (chunkSize > kMaxChunkSize) {
    auto msg = fmt::format("chunk size too large: {}", chunkSize);
    XLOG(ERR, msg);
    return makeError(StorageCode::kChunkStoreInitFailed, std::move(msg));
  }
  auto dirPath = path_ / Size::toString(chunkSize);
  boost::system::error_code ec{};
  boost::filesystem::create_directories(dirPath, ec);
  if (UNLIKELY(ec.failed())) {
    XLOGF(ERR, "chunk store create directory {} failed: {}", dirPath.string(), ec.message());
    return makeError(StorageCode::kChunkOpenFailed,
                     fmt::format("chunk store create directory {} failed: {}", dirPath.string(), ec.message()));
  }

  for (auto i = 0u; i < physicalFileCount_; ++i) {
    ChunkFileId fileId;
    fileId.chunkSize = chunkSize;
    fileId.chunkIdx = i;
    RETURN_AND_LOG_ON_ERROR(openInnerFile(fileId, true));
  }
  return Void{};
}

}  // namespace hf3fs::storage
