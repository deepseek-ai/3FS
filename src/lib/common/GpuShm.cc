#include "GpuShm.h"

#include <cstring>
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef HF3FS_GDR_ENABLED
#include <cuda_runtime.h>
#endif

#include "common/net/ib/RDMABuf.h"
#include "common/net/ib/RDMABufAccelerator.h"

namespace hf3fs::lib {

// GpuIpcHandle implementation

std::string GpuIpcHandle::serialize() const {
  std::string result(64 + 1, '\0');
  result[0] = valid ? 1 : 0;
  std::memcpy(&result[1], data, 64);
  return result;
}

std::optional<GpuIpcHandle> GpuIpcHandle::deserialize(const std::string &data) {
  if (data.size() != 65) {
    return std::nullopt;
  }

  GpuIpcHandle handle;
  handle.valid = data[0] != 0;
  std::memcpy(handle.data, &data[1], 64);
  return handle;
}

// GpuShmBuf implementation

GpuShmBuf::GpuShmBuf(void *devicePtr, size_t size, int deviceId, meta::Uid u, int pid, int ppid)
    : id(Uuid::random()),
      devicePtr(devicePtr),
      size(size),
      deviceId(deviceId),
      user(u),
      pid(pid),
      ppid(ppid),
      isImported_(false),
      memhs_(size ? 1 : 0) {
  XLOGF(INFO, "Creating GpuShmBuf: ptr={}, size={}, device={}, id={}", devicePtr, size, deviceId, id.toHexString());

  // Get IPC handle for the GPU memory
#ifdef HF3FS_GDR_ENABLED
  if (devicePtr) {
    cudaError_t err = cudaSetDevice(deviceId);
    if (err != cudaSuccess) {
      XLOGF(WARN, "cudaSetDevice({}) failed: {}", deviceId, cudaGetErrorString(err));
      ipcHandle_.valid = false;
    } else {
      cudaIpcMemHandle_t cudaHandle;
      err = cudaIpcGetMemHandle(&cudaHandle, devicePtr);
      if (err == cudaSuccess) {
        std::memcpy(ipcHandle_.data, &cudaHandle, sizeof(cudaHandle));
        ipcHandle_.valid = true;
      } else {
        XLOGF(WARN, "cudaIpcGetMemHandle failed: {}", cudaGetErrorString(err));
        ipcHandle_.valid = false;
      }
    }
  }
#else
  ipcHandle_.valid = false;  // No CUDA runtime (HF3FS_GDR_ENABLED not set)
#endif

  // RDMA registration is owned by RDMABufAccelerator in memh().
}

GpuShmBuf::GpuShmBuf(const GpuIpcHandle &ipcHandle, size_t size, int deviceId, Uuid id)
    : id(id),
      devicePtr(nullptr),
      size(size),
      deviceId(deviceId),
      user(meta::Uid(0)),
      pid(0),
      ppid(0),
      isImported_(true),
      ipcHandle_(ipcHandle),
      memhs_(size ? 1 : 0) {
  XLOGF(INFO, "Importing GpuShmBuf: size={}, device={}, id={}", size, deviceId, id.toHexString());

  if (!ipcHandle.valid) {
    XLOGF(WARN, "IPC handle not valid for import");
    return;
  }

  // Import the GPU memory via IPC handle
#ifdef HF3FS_GDR_ENABLED
  cudaError_t err = cudaSetDevice(deviceId);
  if (err != cudaSuccess) {
    XLOGF(ERR, "cudaSetDevice({}) failed: {}", deviceId, cudaGetErrorString(err));
    return;
  }

  cudaIpcMemHandle_t cudaHandle;
  std::memcpy(&cudaHandle, ipcHandle.data, sizeof(cudaHandle));
  err = cudaIpcOpenMemHandle(&importedPtr_, cudaHandle, cudaIpcMemLazyEnablePeerAccess);
  if (err != cudaSuccess) {
    XLOGF(ERR, "cudaIpcOpenMemHandle failed: {}", cudaGetErrorString(err));
    importedPtr_ = nullptr;
    return;
  }
  devicePtr = importedPtr_;
#else
  XLOGF(ERR, "GPU IPC import requires CUDA runtime (HF3FS_GDR_ENABLED not set)");
  importedPtr_ = nullptr;
  return;
#endif

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
  // Close IPC handle after all MR/IOBuffer owners have been released.
  if (isImported_ && importedPtr_) {
    void *ptr = importedPtr_;
    importedPtr_ = nullptr;
    devicePtr = nullptr;
#ifdef HF3FS_GDR_ENABLED
    cudaError_t err = cudaSetDevice(deviceId);
    if (err != cudaSuccess) {
      XLOGF(WARN, "cudaSetDevice({}) failed: {}", deviceId, cudaGetErrorString(err));
    }
    err = cudaIpcCloseMemHandle(ptr);
    if (err != cudaSuccess) {
      XLOGF(WARN, "cudaIpcCloseMemHandle failed: {}", cudaGetErrorString(err));
    }
#else
    XLOGF(DBG, "Closing imported GPU IPC handle");
#endif
  }
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

std::optional<GpuIpcHandle> GpuShmBuf::getIpcHandle() const {
  if (ipcHandle_.valid) {
    return ipcHandle_;
  }

  // Try to get IPC handle if we have a device pointer
  if (devicePtr && !isImported_) {
#ifdef HF3FS_GDR_ENABLED
    cudaError_t err = cudaSetDevice(deviceId);
    if (err != cudaSuccess) {
      XLOGF(WARN, "cudaSetDevice({}) failed: {}", deviceId, cudaGetErrorString(err));
    } else {
      cudaIpcMemHandle_t cudaHandle;
      err = cudaIpcGetMemHandle(&cudaHandle, devicePtr);
      if (err == cudaSuccess) {
        GpuIpcHandle handle;
        std::memcpy(handle.data, &cudaHandle, sizeof(cudaHandle));
        handle.valid = true;
        return handle;
      }
      XLOGF(WARN, "cudaIpcGetMemHandle failed: {}", cudaGetErrorString(err));
    }
#else
    XLOGF(WARN, "IPC handle export requires CUDA runtime");
#endif
  }

  return std::nullopt;
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

// GpuIpcChannel implementation

class GpuIpcChannel::Impl {
 public:
  ~Impl() {
    if (peerFd_ >= 0) {
      close(peerFd_);
    }
    if (fd_ >= 0) {
      close(fd_);
    }
    if (isServer_ && !path_.empty()) {
      unlink(path_.c_str());
    }
  }

  bool init(const Path &path, bool isServer) {
    path_ = path.string();
    isServer_ = isServer;

    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
      XLOGF(ERR, "Failed to create socket: {}", strerror(errno));
      return false;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

    if (isServer) {
      unlink(path_.c_str());
      if (bind(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        XLOGF(ERR, "Failed to bind socket: {}", strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
      }
      if (listen(fd_, 1) < 0) {
        XLOGF(ERR, "Failed to listen on socket: {}", strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
      }
    } else {
      if (connect(fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        XLOGF(ERR, "Failed to connect to socket: {}", strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
      }
    }

    return true;
  }

  bool sendHandle(const GpuIpcHandle &handle, const Uuid &id, size_t size, int deviceId) {
    if (!ensureConnected()) {
      return false;
    }
    int ioFd = isServer_ ? peerFd_ : fd_;

    // Protocol: [64 bytes handle][16 bytes uuid][8 bytes size][4 bytes deviceId][1 byte valid]
    uint8_t buf[64 + 16 + 8 + 4 + 1];
    std::memcpy(buf, handle.data, 64);
    std::memcpy(buf + 64, id.data, 16);
    std::memcpy(buf + 80, &size, 8);
    std::memcpy(buf + 88, &deviceId, 4);
    buf[92] = handle.valid ? 1 : 0;

    ssize_t sent = write(ioFd, buf, sizeof(buf));
    return sent == sizeof(buf);
  }

  bool recvHandle(GpuIpcHandle &handle, Uuid &id, size_t &size, int &deviceId, int timeout) {
    if (!ensureConnected()) {
      return false;
    }
    int ioFd = isServer_ ? peerFd_ : fd_;

    // TODO: Implement timeout using poll/select
    (void)timeout;

    uint8_t buf[64 + 16 + 8 + 4 + 1];
    ssize_t received = read(ioFd, buf, sizeof(buf));
    if (received != sizeof(buf)) {
      return false;
    }

    std::memcpy(handle.data, buf, 64);
    std::memcpy(id.data, buf + 64, 16);
    std::memcpy(&size, buf + 80, 8);
    std::memcpy(&deviceId, buf + 88, 4);
    handle.valid = buf[92] != 0;

    return true;
  }

 private:
  bool ensureConnected() {
    if (fd_ < 0) {
      return false;
    }
    if (!isServer_) {
      return true;
    }
    if (peerFd_ >= 0) {
      return true;
    }
    peerFd_ = accept(fd_, nullptr, nullptr);
    if (peerFd_ < 0) {
      XLOGF(ERR, "Failed to accept GPU IPC connection: {}", strerror(errno));
      return false;
    }
    return true;
  }

  int fd_ = -1;
  int peerFd_ = -1;
  std::string path_;
  bool isServer_ = false;
};

std::unique_ptr<GpuIpcChannel> GpuIpcChannel::createServer(const Path &path) {
  auto channel = std::unique_ptr<GpuIpcChannel>(new GpuIpcChannel());
  channel->impl_ = std::make_unique<Impl>();
  if (!channel->impl_->init(path, true)) {
    return nullptr;
  }
  return channel;
}

std::unique_ptr<GpuIpcChannel> GpuIpcChannel::createClient(const Path &path) {
  auto channel = std::unique_ptr<GpuIpcChannel>(new GpuIpcChannel());
  channel->impl_ = std::make_unique<Impl>();
  if (!channel->impl_->init(path, false)) {
    return nullptr;
  }
  return channel;
}

GpuIpcChannel::~GpuIpcChannel() = default;

bool GpuIpcChannel::sendHandle(const GpuIpcHandle &handle, const Uuid &id, size_t size, int deviceId) {
  return impl_->sendHandle(handle, id, size, deviceId);
}

bool GpuIpcChannel::recvHandle(GpuIpcHandle &handle, Uuid &id, size_t &size, int &deviceId, int timeout) {
  return impl_->recvHandle(handle, id, size, deviceId, timeout);
}

// GpuShmBufTable implementation

Result<int> GpuShmBufTable::add(std::shared_ptr<GpuShmBuf> buf) {
  if (!buf) {
    return makeError(StatusCode::kInvalidArg, "Null buffer");
  }

  std::lock_guard<std::mutex> lock(mutex_);

  // Check if already registered
  auto it = idToIndex_.find(buf->id);
  if (it != idToIndex_.end()) {
    return it->second;
  }

  int index = buffers_.size();
  buffers_.push_back(buf);
  idToIndex_[buf->id] = index;

  return index;
}

void GpuShmBufTable::remove(int index) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (index < 0 || static_cast<size_t>(index) >= buffers_.size()) {
    return;
  }

  auto &buf = buffers_[index];
  if (buf) {
    idToIndex_.erase(buf->id);
    buf.reset();
  }
}

std::shared_ptr<GpuShmBuf> GpuShmBufTable::get(int index) const {
  std::lock_guard<std::mutex> lock(mutex_);

  if (index < 0 || static_cast<size_t>(index) >= buffers_.size()) {
    return nullptr;
  }

  return buffers_[index];
}

std::shared_ptr<GpuShmBuf> GpuShmBufTable::findById(const Uuid &id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = idToIndex_.find(id);
  if (it != idToIndex_.end() && it->second < static_cast<int>(buffers_.size())) {
    return buffers_[it->second];
  }

  return nullptr;
}

std::shared_ptr<GpuShmBuf> GpuShmBufTable::findByPtr(void *devicePtr) const {
  std::lock_guard<std::mutex> lock(mutex_);

  for (const auto &buf : buffers_) {
    if (buf && buf->devicePtr == devicePtr) {
      return buf;
    }
  }

  return nullptr;
}

std::vector<std::shared_ptr<GpuShmBuf>> GpuShmBufTable::getByDevice(int deviceId) const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::shared_ptr<GpuShmBuf>> result;
  for (const auto &buf : buffers_) {
    if (buf && buf->deviceId == deviceId) {
      result.push_back(buf);
    }
  }

  return result;
}

size_t GpuShmBufTable::size() const {
  std::lock_guard<std::mutex> lock(mutex_);

  size_t count = 0;
  for (const auto &buf : buffers_) {
    if (buf) ++count;
  }

  return count;
}

}  // namespace hf3fs::lib
