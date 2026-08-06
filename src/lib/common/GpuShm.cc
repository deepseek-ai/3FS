#include "lib/common/GpuShm.h"

#include "common/net/ib/RDMABuf.h"

namespace hf3fs::lib {

Result<std::shared_ptr<GpuShmBuf>> GpuShmBuf::create(const CudaIpcHandle &ipcHandle,
                                                     size_t allocationSize,
                                                     size_t offset,
                                                     size_t size,
                                                     int deviceId) {
  auto imported = CudaIpcMapping::open(deviceId, ipcHandle, allocationSize);
  RETURN_ON_ERROR(imported);

  auto mapping = std::make_shared<CudaIpcMapping>(std::move(*imported));
  auto view = mapping->view(offset, size);
  RETURN_ON_ERROR(view);

  std::shared_ptr<void> backingOwner = mapping;
  auto rdmaBuf =
      net::RDMABuf::createFromCudaBuffer(static_cast<uint8_t *>(*view), size, deviceId, std::move(backingOwner));
  RETURN_ON_ERROR(rdmaBuf);

  auto ioBuffer = storage::client::IOBuffer(std::move(*rdmaBuf));
  return std::shared_ptr<GpuShmBuf>(new GpuShmBuf(size, std::move(ioBuffer)));
}

CoTryTask<storage::client::IOBuffer *> GpuShmBufForIO::memh(size_t len) const {
  if (!buf_ || off_ > buf_->size || len > buf_->size - off_) {
    co_return makeError(StatusCode::kInvalidArg, "invalid GPU buf off and/or io len");
  }
  co_return buf_->ioBuffer();
}

}  // namespace hf3fs::lib
