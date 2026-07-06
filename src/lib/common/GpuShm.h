#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <folly/experimental/coro/Baton.h>

#include "client/storage/StorageClient.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Path.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Schema.h"
#include "lib/common/Shm.h"

namespace hf3fs::lib {

/**
 * GPU IPC Memory Handle
 *
 * Wrapper for CUDA IPC memory handle that allows GPU memory to be
 * shared across processes. This is essential for the fuse daemon
 * to access GPU memory allocated by the inference engine.
 */
struct GpuIpcHandle {
  uint8_t data[64];  // Same size as cudaIpcMemHandle_t
  bool valid = false;

  GpuIpcHandle() = default;

  // Serialize for transmission
  std::string serialize() const;
  static std::optional<GpuIpcHandle> deserialize(const std::string& data);
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
 * Usage scenarios:
 * 1. Inference engine allocates GPU memory and creates GpuShmBuf
 * 2. IPC handle is shared with fuse daemon
 * 3. Fuse daemon imports the handle and registers for RDMA
 * 4. Storage I/O goes directly to GPU memory via GDR
 */
struct GpuShmBuf : public std::enable_shared_from_this<GpuShmBuf> {
  /**
   * Create from existing GPU device pointer (owner process)
   *
   * The caller retains ownership of the GPU memory.
   *
   * @param devicePtr GPU device pointer
   * @param size Size in bytes
   * @param deviceId CUDA device ID
   * @param u Owner user ID
   * @param pid Owner process ID
   * @param ppid Owner parent process ID
   */
  GpuShmBuf(void* devicePtr,
            size_t size,
            int deviceId,
            meta::Uid u,
            int pid,
            int ppid);

  /**
   * Create by importing from IPC handle (consumer process)
   *
   * @param ipcHandle CUDA IPC memory handle
   * @param size Size in bytes
   * @param deviceId CUDA device ID to use for import
   * @param id UUID identifying this buffer
   */
  GpuShmBuf(const GpuIpcHandle& ipcHandle,
            size_t size,
            int deviceId,
            Uuid id);

  ~GpuShmBuf();

  // Non-copyable
  GpuShmBuf(const GpuShmBuf&) = delete;
  GpuShmBuf& operator=(const GpuShmBuf&) = delete;

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
  CoTask<void> registerForIO(
      folly::Executor::KeepAlive<> exec,
      storage::client::StorageClient& sc,
      std::function<void()>&& recordMetrics);

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
  bool checkId(const Uuid& uid) const { return id == uid; }

  /**
   * Get the IPC handle for sharing with other processes
   *
   * @return IPC handle if available
   */
  std::optional<GpuIpcHandle> getIpcHandle() const;

  /**
   * Synchronize GPU memory for RDMA operations
   *
   * @param direction 0 = before RDMA, 1 = after RDMA
   */
  void sync(int direction) const;

  /**
   * Check if this is an imported buffer (vs. owned)
   */
  bool isImported() const { return isImported_; }

  // Public fields (matching ShmBuf interface where applicable)
  Uuid id;
  void* devicePtr = nullptr;
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
  bool isImported_ = false;
  bool isRegistered_ = false;
  void* importedPtr_ = nullptr;  // Pointer from cudaIpcOpenMemHandle

  GpuIpcHandle ipcHandle_;

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
  uint8_t* ptr() const {
    return static_cast<uint8_t*>(buf_->devicePtr) + off_;
  }

  /**
   * Get memory handle for I/O
   */
  CoTryTask<storage::client::IOBuffer*> memh(size_t len) const;

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

/**
 * IPC Channel for GPU memory sharing
 *
 * Experimental/internal: current GDR v1 publishes GPU iovs through the
 * existing FUSE iov table and strict GdrUri keys. This channel is not on
 * that main path.
 *
 * Provides a mechanism for transferring GPU IPC handles between
 * processes (e.g., inference engine to fuse daemon).
 */
class GpuIpcChannel {
 public:
  /**
   * Create the server side of the channel
   *
   * @param path Path for the IPC endpoint
   * @return Created channel or nullptr on error
   */
  static std::unique_ptr<GpuIpcChannel> createServer(const Path& path);

  /**
   * Create the client side of the channel
   *
   * @param path Path to the server endpoint
   * @return Created channel or nullptr on error
   */
  static std::unique_ptr<GpuIpcChannel> createClient(const Path& path);

  ~GpuIpcChannel();

  /**
   * Send an IPC handle to the peer
   *
   * @param handle The IPC handle to send
   * @param id UUID identifying the buffer
   * @param size Buffer size
   * @param deviceId GPU device ID
   * @return true on success
   */
  bool sendHandle(const GpuIpcHandle& handle,
                  const Uuid& id,
                  size_t size,
                  int deviceId);

  /**
   * Receive an IPC handle from the peer
   *
   * @param handle Output parameter for the received handle
   * @param id Output parameter for buffer UUID
   * @param size Output parameter for buffer size
   * @param deviceId Output parameter for GPU device ID
   * @param timeout Timeout in milliseconds (-1 for blocking)
   * @return true on success
   */
  bool recvHandle(GpuIpcHandle& handle,
                  Uuid& id,
                  size_t& size,
                  int& deviceId,
                  int timeout = -1);

 private:
  GpuIpcChannel() = default;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

/**
 * Table for managing GPU shared memory buffers
 *
 * Similar to ProcShmBuf but for GPU memory.
 */
class GpuShmBufTable {
 public:
  /**
   * Register a GPU buffer
   *
   * @param buf The buffer to register
   * @return Index or error
   */
  Result<int> add(std::shared_ptr<GpuShmBuf> buf);

  /**
   * Remove a GPU buffer
   *
   * @param index Index of the buffer to remove
   */
  void remove(int index);

  /**
   * Get a buffer by index
   */
  std::shared_ptr<GpuShmBuf> get(int index) const;

  /**
   * Find a buffer by ID
   */
  std::shared_ptr<GpuShmBuf> findById(const Uuid& id) const;

  /**
   * Find a buffer by device pointer
   */
  std::shared_ptr<GpuShmBuf> findByPtr(void* devicePtr) const;

  /**
   * Get all buffers for a specific device
   */
  std::vector<std::shared_ptr<GpuShmBuf>> getByDevice(int deviceId) const;

  /**
   * Get the total number of buffers
   */
  size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<GpuShmBuf>> buffers_;
  std::unordered_map<Uuid, int> idToIndex_;
};

}  // namespace hf3fs::lib
