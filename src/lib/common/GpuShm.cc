#include "GpuShm.h"

#include <algorithm>
#include <cstring>
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>

#include "common/net/ib/RDMABufAccelerator.h"

namespace hf3fs::lib {

// GpuShmBuf implementation

GpuShmBuf::GpuShmBuf(const GpuIpcHandle &ipcHandle,
                     size_t allocationSize,
                     size_t offset,
                     size_t size,
                     int deviceId,
                     Uuid id)
    : id(id),
      allocationSize(allocationSize),
      offset(offset),
      devicePtr(nullptr),
      size(size),
      deviceId(deviceId),
      user(meta::Uid(0)),
      pid(0),
      ppid(0),
      memhs_(size ? 1 : 0) {
  XLOGF(INFO, "Importing GpuShmBuf: size={}, device={}, id={}", size, deviceId, id.toHexString());

  if (!ipcHandle.valid) {
    importError_.emplace(StatusCode::kInvalidArg, "CUDA IPC handle is not valid");
    XLOGF(WARN,
          "Failed to import GpuShmBuf id={} device={} allocationSize={} offset={} size={}: {}",
          id.toHexString(),
          deviceId,
          allocationSize,
          offset,
          size,
          *importError_);
    return;
  }

  CudaIpcHandle cudaHandle;
  std::memcpy(cudaHandle.data(), ipcHandle.data, cudaHandle.size());
  auto mapping = CudaIpcMapping::open(deviceId, cudaHandle, allocationSize);
  if (!mapping) {
    importError_ = mapping.error();
    XLOGF(ERR,
          "Failed to import GpuShmBuf id={} device={} allocationSize={} offset={} size={}: {}",
          id.toHexString(),
          deviceId,
          allocationSize,
          offset,
          size,
          *importError_);
    return;
  }
  auto view = mapping->view(offset, size);
  if (!view) {
    importError_ = view.error();
    XLOGF(ERR,
          "Failed to create GpuShmBuf view id={} device={} allocationSize={} offset={} size={}: {}",
          id.toHexString(),
          deviceId,
          allocationSize,
          offset,
          size,
          *importError_);
    return;
  }

  allocationBase = mapping->allocationBase();
  devicePtr = *view;
  importedMapping_ = std::make_unique<CudaIpcMapping>(std::move(*mapping));

  // RDMA registration is owned by RDMABufAccelerator in memh().
}

GpuShmBuf::~GpuShmBuf() {
  XLOGF(DBG, "Destroying GpuShmBuf: id={}", id.toHexString());

  // Deregister from I/O if registered
  if (isRegistered_) {
    // Note: Should call deregisterForIO() but it's a coroutine
    XLOGF(WARN, "GpuShmBuf destroyed while still registered for I/O");
  }

  for (auto &memh : memhs_) {
    memh.store(nullptr);
  }
  isRegistered_ = false;

  if (devicePtr) {
    auto *cache = net::GDRManager::instance().getRegionCache();
    if (cache) {
      cache->invalidate(devicePtr);
    }
  }
  // Close the imported allocation only after all MR/IOBuffer owners are gone.
  devicePtr = nullptr;
  allocationBase = nullptr;
  importedMapping_.reset();
}

CoTask<void> GpuShmBuf::registerForIO(folly::Executor::KeepAlive<> exec,
                                      storage::client::StorageClient &sc,
                                      std::function<void()> &&recordMetrics) {
  (void)exec;
  (void)sc;

  if (isRegistered_) {
    co_return;
  }

  if (!devicePtr || size == 0) {
    XLOGF(ERR, "Cannot register invalid GpuShmBuf for I/O");
    co_return;
  }

  bool expected = false;
  if (!regging_.compare_exchange_strong(expected, true)) {
    // Another registration is in progress, wait for it
    co_await memhBaton_;
    co_return;
  }

  SCOPE_EXIT {
    regging_.store(false);
    memhBaton_.post();
  };

  XLOGF(DBG, "Registering GpuShmBuf for I/O: ptr={}, size={}", devicePtr, size);

  for (auto &memh : memhs_) {
    memh.store(nullptr);
  }

  isRegistered_ = true;

  if (recordMetrics) {
    recordMetrics();
  }

  XLOGF(INFO, "GpuShmBuf registered for I/O: blocks={}", memhs_.size());
  co_return;
}

CoTask<std::shared_ptr<storage::client::IOBuffer>> GpuShmBuf::memh(size_t off) {
  if (!isRegistered_) {
    XLOGF(ERR, "GpuShmBuf not registered for I/O");
    co_return nullptr;
  }

  // Calculate block index
  size_t blockSize = size;  // Using full size as single block for now
  size_t blockIndex = off / blockSize;

  if (blockIndex >= memhs_.size()) {
    XLOGF(ERR, "Offset {} out of range for GpuShmBuf", off);
    co_return nullptr;
  }

  auto memh = memhs_[blockIndex].load();
  if (memh) {
    co_return memh;
  }

  // Create IOBuffer via RDMABufAccelerator for proper GPU RDMA registration
  auto blockPtr = static_cast<uint8_t *>(devicePtr) + blockIndex * blockSize;
  auto blockLen = std::min(blockSize, size - blockIndex * blockSize);
  auto gpuBuf = net::RDMABufAccelerator::createFromGpuPointer(blockPtr, blockLen, deviceId);
  if (!gpuBuf.valid()) {
    XLOGF(ERR, "Failed to register GPU memory via RDMABufAccelerator for block {}", blockIndex);
    co_return nullptr;
  }

  auto ioBuffer = std::make_shared<storage::client::IOBuffer>(net::RDMABufUnified(std::move(gpuBuf)));
  memhs_[blockIndex].store(ioBuffer);

  co_return ioBuffer;
}

CoTask<void> GpuShmBuf::deregisterForIO() {
  if (!isRegistered_) {
    co_return;
  }

  XLOGF(DBG, "Deregistering GpuShmBuf from I/O");

  for (auto &memh : memhs_) {
    memh.store(nullptr);
  }
  isRegistered_ = false;

  co_return;
}

void GpuShmBuf::sync(int direction) const {
  XLOGF(DBG, "GPU sync: direction={}, ptr={}, size={}", direction, devicePtr, size);
}

// GpuShmBufForIO implementation

CoTryTask<storage::client::IOBuffer *> GpuShmBufForIO::memh(size_t len) const {
  XLOGF(DBG, "GpuShmBufForIO::memh: off={}, len={}", off_, len);
  if (!buf_ || off_ > buf_->size || len > buf_->size - off_) {
    co_return makeError(StatusCode::kInvalidArg, "invalid GPU buf off and/or io len");
  }

  auto result = co_await buf_->memh(off_);
  if (!result) {
    co_return makeError(StatusCode::kIOError, "Failed to get GPU memory handle");
  }

  co_return result.get();
}

}  // namespace hf3fs::lib
