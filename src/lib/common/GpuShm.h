#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <folly/concurrency/AtomicSharedPtr.h>
#include <folly/experimental/coro/Baton.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "client/storage/StorageClient.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Schema.h"
#include "lib/common/CudaIpcMemory.h"

namespace hf3fs::lib {

/**
 * GPU IPC Memory Handle
 *
 * Wrapper for CUDA IPC memory handle that allows GPU memory to be
 * shared across processes. This is essential for the fuse daemon
 * to access GPU memory allocated by the inference engine.
 */
struct GpuIpcHandle {
  uint8_t data[kCudaIpcHandleBytes];
  bool valid = false;

  GpuIpcHandle() = default;
};

/**
 * GPU Shared Memory Buffer
 *
 * Extension of ShmBuf concept for GPU memory. Instead of using POSIX
 * shared memory, this uses CUDA IPC handles to share GPU memory
 * between processes.
 *
 * Key differences from ShmBuf:
 * - Memory resides on GPU device
 * - Uses CUDA IPC for cross-process sharing
 * - Requires RDMA GDR registration for storage I/O
 * - May need CUDA context management
 *
 * The fuse daemon imports the handle published by the client and registers
 * the resulting device pointer for direct storage-to-GPU I/O.
 */
struct GpuShmBuf : public std::enable_shared_from_this<GpuShmBuf> {
  /**
   * Create by importing from IPC handle (consumer process)
   *
   * @param ipcHandle CUDA IPC memory handle
   * @param allocationSize Full exported CUDA allocation size
   * @param offset View offset within the allocation
   * @param size View size in bytes
   * @param deviceId CUDA device ID to use for import
   * @param id UUID identifying this buffer
   */
  GpuShmBuf(const GpuIpcHandle &ipcHandle, size_t allocationSize, size_t offset, size_t size, int deviceId, Uuid id);

  ~GpuShmBuf();

  // Non-copyable
  GpuShmBuf(const GpuShmBuf &) = delete;
  GpuShmBuf &operator=(const GpuShmBuf &) = delete;

  /**
   * Register this GPU buffer for I/O operations
   *
   * This registers the GPU memory with the RDMA subsystem via GDR,
   * enabling direct storage-to-GPU data transfers.
   *
   * @param exec Executor for async operations
   * @param sc Storage client for RDMA operations
   * @param recordMetrics Callback for metrics recording
   */
  CoTask<void> registerForIO(folly::Executor::KeepAlive<> exec,
                             storage::client::StorageClient &sc,
                             std::function<void()> &&recordMetrics);

  /**
   * Get memory handle for I/O at given offset
   *
   * @param off Offset within the buffer
   * @return IOBuffer for storage operations
   */
  CoTask<std::shared_ptr<storage::client::IOBuffer>> memh(size_t off);

  /**
   * Deregister from I/O subsystem
   */
  CoTask<void> deregisterForIO();

  /**
   * Check if the buffer ID matches
   */
  bool checkId(const Uuid &uid) const { return id == uid; }

  /** Detailed CUDA IPC import failure, if construction did not produce a usable mapping. */
  const std::optional<Status> &importError() const { return importError_; }

  /**
   * Synchronize GPU memory for RDMA operations
   *
   * @param direction 0 = before RDMA, 1 = after RDMA
   */
  void sync(int direction) const;

  // Public fields (matching ShmBuf interface where applicable)
  Uuid id;
  void *allocationBase = nullptr;
  size_t allocationSize = 0;
  size_t offset = 0;
  void *devicePtr = nullptr;
  size_t size = 0;
  int deviceId = -1;

  // For access control
  meta::Uid user{0};
  int pid = 0;
  int ppid = 0;

  // For fuse integration
  std::string key;
  int iorIndex = -1;
  bool isIoRing = false;
  bool forRead = true;
  int ioDepth = 0;

 private:
  bool isRegistered_ = false;
  std::optional<Status> importError_;
  std::unique_ptr<CudaIpcMapping> importedMapping_;

  // For I/O registration
  std::vector<folly::atomic_shared_ptr<storage::client::IOBuffer>> memhs_;
  folly::coro::Baton memhBaton_;
  std::atomic<bool> regging_{false};
};

/**
 * GPU Shared Memory Buffer for I/O
 *
 * Wrapper for GpuShmBuf that provides offset-based access,
 * similar to ShmBufForIO.
 *
 */
class GpuShmBufForIO {
 public:
  GpuShmBufForIO(std::shared_ptr<GpuShmBuf> buf, size_t off)
      : buf_(std::move(buf)),
        off_(off) {}

  /**
   * Get pointer to the data at offset
   */
  uint8_t *ptr() const { return static_cast<uint8_t *>(buf_->devicePtr) + off_; }

  /**
   * Get memory handle for I/O
   */
  CoTryTask<storage::client::IOBuffer *> memh(size_t len) const;

  /**
   * Get the underlying GpuShmBuf
   */
  std::shared_ptr<GpuShmBuf> buffer() const { return buf_; }

  /**
   * Get the offset within the buffer
   */
  size_t offset() const { return off_; }

 private:
  std::shared_ptr<GpuShmBuf> buf_;
  size_t off_;
};

}  // namespace hf3fs::lib
