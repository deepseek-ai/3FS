#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "client/storage/StorageClient.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "lib/common/CudaIpcMemory.h"

namespace hf3fs::lib {

/** A CUDA IPC view imported and registered once for the lifetime of an IOV entry. */
class GpuShmBuf {
 public:
  static Result<std::shared_ptr<GpuShmBuf>> create(const CudaIpcHandle &ipcHandle,
                                                   size_t allocationSize,
                                                   size_t offset,
                                                   size_t size,
                                                   int deviceId);

  uint8_t *dataAtOffset(size_t offset) const { return ioBuffer_.dataAtOffset(offset); }
  storage::client::IOBuffer *ioBuffer() { return &ioBuffer_; }

  const size_t size;

 private:
  GpuShmBuf(size_t size, storage::client::IOBuffer ioBuffer)
      : size(size),
        ioBuffer_(std::move(ioBuffer)) {}

  storage::client::IOBuffer ioBuffer_;
};

/** Offset view used by the existing host/GPU IOV dispatch in the FUSE worker. */
class GpuShmBufForIO {
 public:
  GpuShmBufForIO(std::shared_ptr<GpuShmBuf> buf, size_t off)
      : buf_(std::move(buf)),
        off_(off) {}

  uint8_t *ptr() const { return buf_ ? buf_->dataAtOffset(off_) : nullptr; }
  CoTryTask<storage::client::IOBuffer *> memh(size_t len) const;

 private:
  std::shared_ptr<GpuShmBuf> buf_;
  size_t off_;
};

}  // namespace hf3fs::lib
