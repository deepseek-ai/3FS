#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/net/ib/AcceleratorMemory.h"
#include "common/utils/Result.h"

namespace hf3fs::net {

/**
 * GPU Memory Import Support
 *
 * Experimental/internal: the production GDR v1 path uses GpuShmBuf,
 * GdrUri, and RDMABufAccelerator directly. Keep this bridge out of the
 * main path until it has a concrete call site and ownership contract.
 *
 * This module provides mechanisms for importing GPU memory from external
 * processes and registering it for RDMA operations. This is essential for
 * scenarios where:
 *
 * 1. The usrbio client runs in the inference engine process
 * 2. GPU memory is allocated with the inference engine's CUDA context
 * 3. The fuse daemon needs to register this memory for RDMA to storage
 *
 * The key challenge is CUDA context ownership: GPU memory is associated with
 * a specific CUDA context, and operations on that memory must be performed
 * from within that context. However, for GDR to work, the RDMA driver needs
 * to be able to access the GPU memory from the fuse daemon process.
 *
 * Solutions supported:
 *
 * 1. CUDA IPC (Inter-Process Communication)
 *    - cudaIpcGetMemHandle() exports memory from owner process
 *    - cudaIpcOpenMemHandle() imports memory in consumer process
 *    - Consumer gets a device pointer valid in their CUDA context
 *    - Memory can then be registered with RDMA
 *
 * 2. Nvidia peermem (BAR1 mapping)
 *    - nvidia_peermem kernel module
 *    - Direct mapping of GPU memory for peer access
 *    - Requires same machine (no cross-node)
 */

/**
 * Import method to use for GPU memory
 */
enum class AcceleratorImportMethod {
  Auto,       // Automatically choose best method
  CudaIpc,    // CUDA IPC handles
  DirectReg,  // Direct registration (nvidia_peermem)
};

/**
 * Configuration for GPU memory import
 */
class AcceleratorImportConfig : public ConfigBase<AcceleratorImportConfig> {
 public:
  CONFIG_ITEM(method, AcceleratorImportMethod::Auto);
  CONFIG_ITEM(enable_peer_access, true);
  CONFIG_ITEM(cache_imported_regions, true);
  CONFIG_ITEM(verify_import_success, true);
};

/**
 * Exported GPU memory handle
 *
 * This structure contains all information needed to import GPU memory
 * in another process. It can be serialized and sent via IPC.
 */
struct AcceleratorExportHandle {
  // CUDA IPC handle (64 bytes)
  uint8_t ipcHandle[64];
  bool hasIpcHandle = false;

  // Memory info
  uint64_t devicePtrValue = 0;  // Original device pointer (as integer)
  size_t size = 0;
  int deviceId = -1;
  size_t alignment = 0;

  // Serialization
  std::string serialize() const;
   static Result<AcceleratorExportHandle> deserialize(const std::string& data);
};

/**
 * Imported GPU memory region
 *
 * Represents GPU memory that has been imported from another process
 * and registered with the RDMA subsystem.
 */
class AcceleratorImportedRegion {
 public:
  ~AcceleratorImportedRegion();

  // Non-copyable
  AcceleratorImportedRegion(const AcceleratorImportedRegion&) = delete;
  AcceleratorImportedRegion& operator=(const AcceleratorImportedRegion&) = delete;

  // Movable
  AcceleratorImportedRegion(AcceleratorImportedRegion&&) noexcept;
  AcceleratorImportedRegion& operator=(AcceleratorImportedRegion&&) noexcept;

  /**
   * Create by importing from export handle
   *
   * @param handle Export handle from the owning process
   * @param config Import configuration
   * @return Imported region or error
   */
   static Result<std::unique_ptr<AcceleratorImportedRegion>> import(
       const AcceleratorExportHandle& handle,
       const AcceleratorImportConfig& config = AcceleratorImportConfig());

  // Accessors
  void* ptr() const { return importedPtr_; }
  size_t size() const { return size_; }
  int deviceId() const { return deviceId_; }
   AcceleratorImportMethod method() const { return method_; }

  /**
   * Get the underlying GPU memory region for RDMA operations
   */
  std::shared_ptr<AcceleratorMemoryRegion> getRegion() const { return region_; }

  /**
   * Get memory region for specific IB device
   */
  ibv_mr* getMR(int devId) const {
    return region_ ? region_->getMR(devId) : nullptr;
  }

  /**
   * Get rkey for specific IB device
   */
  std::optional<uint32_t> getRkey(int devId) const {
    return region_ ? region_->getRkey(devId) : std::nullopt;
  }

 private:
  AcceleratorImportedRegion() = default;

   Result<Void> doImport(const AcceleratorExportHandle& handle, const AcceleratorImportConfig& config);
  void cleanup();

  void* importedPtr_ = nullptr;
  size_t size_ = 0;
  int deviceId_ = -1;
   AcceleratorImportMethod method_ = AcceleratorImportMethod::Auto;

  // Resources to cleanup
  bool ownsIpcHandle_ = false;

  // RDMA region
  std::shared_ptr<AcceleratorMemoryRegion> region_;
};

/**
 * GPU Memory Exporter
 *
 * Used by the process that owns the GPU memory to create export handles.
 */
class AcceleratorMemoryExporter {
 public:
  /**
   * Export GPU memory for sharing with other processes
   *
   * @param devicePtr GPU device pointer
   * @param size Size of the memory region
   * @param deviceId CUDA device ID
   * @param method Preferred export method
   * @return Export handle or error
   */
   static Result<AcceleratorExportHandle> exportMemory(
       void* devicePtr,
       size_t size,
       int deviceId,
       AcceleratorImportMethod method = AcceleratorImportMethod::Auto);

  /**
   * Check if CUDA IPC is available
   */
  static bool isCudaIpcSupported();
};

/**
 * GPU Memory Import Manager
 *
 * Manages imported GPU memory regions with caching for efficiency.
 */
class AcceleratorImportManager {
 public:
   static AcceleratorImportManager& instance();

  /**
   * Import GPU memory using the provided handle
   *
   * @param handle Export handle
   * @param config Import configuration
   * @return Imported region
   */
   Result<std::shared_ptr<AcceleratorImportedRegion>> import(
       const AcceleratorExportHandle& handle,
       const AcceleratorImportConfig& config = AcceleratorImportConfig());

  /**
   * Invalidate a cached import by device pointer
   */
  void invalidate(uint64_t devicePtrValue);

  /**
   * Clear all cached imports
   */
  void clear();

  /**
   * Get statistics
   */
  struct Stats {
    size_t activeImports = 0;
    size_t totalImported = 0;
    size_t cacheHits = 0;
    size_t cacheMisses = 0;
  };
  Stats getStats() const;

 private:
  AcceleratorImportManager() = default;

  mutable std::mutex mutex_;
   std::unordered_map<uint64_t, std::weak_ptr<AcceleratorImportedRegion>> cache_;
  Stats stats_;
};

}  // namespace hf3fs::net
