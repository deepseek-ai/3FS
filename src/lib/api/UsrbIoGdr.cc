/**
 * GPU Direct RDMA (GDR) Extension Implementation
 *
 * This implements the simplified GDR API that mirrors the standard usrbio
 * interface. All CUDA complexity is hidden internally.
 */

#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "hf3fs_usrbio.h"

#ifdef HF3FS_GDR_ENABLED
#include <cuda_runtime.h>
#endif

#include "common/logging/LogInit.h"
#include "common/net/ib/AcceleratorMemory.h"
#include "common/net/ib/IBDevice.h"
#include "common/utils/Uuid.h"
#include "lib/common/GdrUri.h"

namespace {

// Magic value to identify GPU iovs (stored in numa field)
constexpr int kGpuIovMagicNuma = -0x6472;  // 0x64='d', 0x72='r' → "dr" for "direct RDMA"

// Forward declaration — used by GpuIovHandle destructor.
void freeGpuMemory(void *devicePtr, int deviceId);

struct GpuIovHandle {
  // GPU memory region registered with RDMA
  std::shared_ptr<hf3fs::net::AcceleratorMemoryRegion> region;

  // GPU device ID
  int deviceId = -1;

  // Original device pointer
  void *devicePtr = nullptr;

  // Whether memory was allocated by this library (needs cudaFree on destroy)
  bool ownsMemory = false;

  // Whether this was imported via IPC
  bool isIpcImported = false;

  // IPC handle for cross-process sharing
  hf3fs::net::AcceleratorMemoryDescriptor::IpcHandle ipcHandle;

  // Memory size
  size_t size = 0;

  ~GpuIovHandle() { release(); }

  void release() {
    if (!devicePtr) {
      region.reset();
      return;
    }

    auto *cache = hf3fs::net::GDRManager::instance().getRegionCache();
    if (cache) {
      cache->invalidate(devicePtr);
    }
    region.reset();

    void *ptr = devicePtr;
    devicePtr = nullptr;
    if (ownsMemory) {
      freeGpuMemory(ptr, deviceId);
    } else if (isIpcImported) {
#ifdef HF3FS_GDR_ENABLED
      cudaError_t err = cudaSetDevice(deviceId);
      if (err != cudaSuccess) {
        XLOGF(WARN, "cudaSetDevice({}) failed: {}", deviceId, cudaGetErrorString(err));
      }
      err = cudaIpcCloseMemHandle(ptr);
      if (err != cudaSuccess) {
        XLOGF(WARN, "cudaIpcCloseMemHandle failed: {}", cudaGetErrorString(err));
      }
#endif
    }
  }
};

// Global registry of GPU iov handles (for tracking and cleanup)
std::mutex gGpuIovMutex;
std::unordered_map<void *, std::unique_ptr<GpuIovHandle>> gGpuIovHandles;

// GDR initialization state
std::once_flag gGdrInitOnce;
bool gGdrInitialized = false;

bool ensureGdrInitialized() {
  std::call_once(gGdrInitOnce, []() {
    // Logging is already initialized by the static Hf3fsInitLib in UsrbIo.cc.
    XLOGF(INFO, "Initializing GDR support");

    // Check IB availability
    if (!hf3fs::net::IBManager::initialized()) {
      XLOGF(WARN, "IB not initialized - GDR may have limited functionality");
    }

    // Initialize GDR manager
    hf3fs::net::GDRConfig config;
    config.set_enabled(true);

    auto result = hf3fs::net::GDRManager::instance().init(config);
    if (!result) {
      XLOGF(ERR, "GDR initialization failed: {}", result.error().message());
      return;
    }

    gGdrInitialized = true;
    XLOGF(INFO, "GDR support initialized successfully");
  });

  return gGdrInitialized;
}

int allocateGpuMemory(size_t size, int deviceId, void **devicePtr) {
  if (!devicePtr || size == 0) {
    return -EINVAL;
  }

  XLOGF(DBG, "Allocating GPU memory: size={}, device={}", size, deviceId);

#ifdef HF3FS_GDR_ENABLED
  cudaError_t err = cudaSetDevice(deviceId);
  if (err != cudaSuccess) {
    XLOGF(ERR, "cudaSetDevice({}) failed: {}", deviceId, cudaGetErrorString(err));
    return -ENODEV;
  }

  err = cudaMalloc(devicePtr, size);
  if (err != cudaSuccess) {
    XLOGF(ERR, "cudaMalloc({}) failed: {}", size, cudaGetErrorString(err));
    return -ENOMEM;
  }

  XLOGF(INFO,
        "Allocated GPU memory: ptr={}, size={}, device={}",
        static_cast<const void *>(*devicePtr),
        size,
        deviceId);
  return 0;
#else
  XLOGF(WARN, "GPU memory allocation requires CUDA runtime - stub implementation");
  *devicePtr = nullptr;
  return -ENOTSUP;
#endif
}

void freeGpuMemory(void *devicePtr, int deviceId) {
  if (!devicePtr) {
    return;
  }

  XLOGF(DBG, "Freeing GPU memory: ptr={}, device={}", static_cast<const void *>(devicePtr), deviceId);

#ifdef HF3FS_GDR_ENABLED
  cudaError_t err = cudaSetDevice(deviceId);
  if (err != cudaSuccess) {
    XLOGF(ERR, "cudaSetDevice({}) failed: {}", deviceId, cudaGetErrorString(err));
    return;
  }
  err = cudaFree(devicePtr);
  if (err != cudaSuccess) {
    XLOGF(ERR, "cudaFree failed: {}", cudaGetErrorString(err));
  }
#endif
}

void registerGpuIov(struct hf3fs_iov *iov, std::unique_ptr<GpuIovHandle> handle) {
  std::lock_guard<std::mutex> lock(gGpuIovMutex);
  gGpuIovHandles[iov->iovh] = std::move(handle);
}

std::unique_ptr<GpuIovHandle> unregisterGpuIov(struct hf3fs_iov *iov) {
  std::lock_guard<std::mutex> lock(gGpuIovMutex);
  auto it = gGpuIovHandles.find(iov->iovh);
  if (it != gGpuIovHandles.end()) {
    auto handle = std::move(it->second);
    gGpuIovHandles.erase(it);
    return handle;
  }
  return nullptr;
}

GpuIovHandle *getGpuHandle(const struct hf3fs_iov *iov) {
  if (!iov || iov->numa != kGpuIovMagicNuma || !iov->iovh) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(gGpuIovMutex);
  auto it = gGpuIovHandles.find(iov->iovh);
  return it != gGpuIovHandles.end() ? it->second.get() : nullptr;
}

int createGpuIovSymlink(const struct hf3fs_iov *iov,
                        int deviceId,
                        const hf3fs::net::AcceleratorMemoryDescriptor::IpcHandle *ipcHandle) {
  hf3fs::Uuid uuid;
  std::memcpy(uuid.data, iov->id, sizeof(uuid.data));

  auto link = fmt::format("{}/3fs-virt/iovs/{}.gdr.d{}", iov->mount_point, uuid.toHexString(), deviceId);

  if (!ipcHandle || !ipcHandle->valid) {
    XLOGF(ERR,
          "Cannot create GDR symlink without valid IPC handle; "
          "cudaIpcGetMemHandle must succeed before registering GPU iov");
    return -EINVAL;
  }

  std::string target = hf3fs::lib::formatGdrUri(deviceId, iov->size, ipcHandle->data, hf3fs::lib::kGdrIpcHandleBytes);
  if (target.empty()) {
    XLOGF(ERR, "Cannot create GDR symlink with invalid target fields: device={}, size={}", deviceId, iov->size);
    return -EINVAL;
  }

  int result = symlink(target.c_str(), link.c_str());
  if (result < 0) {
    XLOGF(WARN, "Failed to create GDR symlink {} -> {}: {}", link, target, strerror(errno));
    return -errno;
  }

  XLOGF(DBG, "Created GDR symlink: {} -> {}", link, target);
  return 0;
}

void removeGpuIovSymlink(const struct hf3fs_iov *iov, int deviceId) {
  hf3fs::Uuid uuid;
  std::memcpy(uuid.data, iov->id, sizeof(uuid.data));

  auto link = fmt::format("{}/3fs-virt/iovs/{}.gdr.d{}", iov->mount_point, uuid.toHexString(), deviceId);

  unlink(link.c_str());
}

}  // namespace

extern "C" {

bool hf3fs_gdr_available(void) {
  if (!ensureGdrInitialized()) {
    return false;
  }
  auto &mgr = hf3fs::net::GDRManager::instance();
  // GDR is only available if initialized AND has a valid region cache
  return mgr.isAvailable() && mgr.getRegionCache() != nullptr;
}

int hf3fs_gdr_device_count(void) {
  if (!ensureGdrInitialized()) {
    return 0;
  }
  return static_cast<int>(hf3fs::net::GDRManager::instance().getGpuDevices().size());
}

int hf3fs_iovcreate_gpu_internal(struct hf3fs_iov *iov,
                                 const char *hf3fs_mount_point,
                                 size_t size,
                                 size_t block_size,
                                 int gpu_device_id) {
  if (!iov || !hf3fs_mount_point || size == 0) {
    return -EINVAL;
  }
  if (block_size != 0) {
    XLOGF(ERR, "GDR iov does not support block_size: {}", block_size);
    return -EINVAL;
  }

  if (!ensureGdrInitialized()) {
    return -ENOTSUP;
  }

  // Validate device ID
  int deviceCount = hf3fs_gdr_device_count();
  if (gpu_device_id < 0 || gpu_device_id >= deviceCount) {
    XLOGF(ERR, "Invalid GPU device ID: {} (have {} devices)", gpu_device_id, deviceCount);
    return -ENODEV;
  }

  // Validate mount point length
  if (strlen(hf3fs_mount_point) >= sizeof(iov->mount_point)) {
    XLOGF(ERR, "Mount point too long: {}", hf3fs_mount_point);
    return -EINVAL;
  }

  XLOGF(DBG, "Creating GPU iov: size={}, device={}", size, gpu_device_id);

  // Allocate GPU memory
  void *devicePtr = nullptr;
  int allocResult = allocateGpuMemory(size, gpu_device_id, &devicePtr);
  if (allocResult != 0) {
    return allocResult;
  }

  // Create internal handle
  auto handle = std::make_unique<GpuIovHandle>();
  handle->deviceId = gpu_device_id;
  handle->devicePtr = devicePtr;
  handle->ownsMemory = true;
  handle->size = size;

  // Create GPU memory descriptor for RDMA registration
  hf3fs::net::AcceleratorMemoryDescriptor desc;
  desc.devicePtr = devicePtr;
  desc.size = size;
  desc.deviceId = gpu_device_id;

  // Register with RDMA subsystem
  auto *cache = hf3fs::net::GDRManager::instance().getRegionCache();
  if (!cache) {
    XLOGF(ERR, "GDR region cache not available");
    return -ENOTSUP;
  }
  auto regionResult = cache->getOrCreate(desc);
  if (!regionResult) {
    XLOGF(ERR, "Failed to register GPU memory with RDMA: {}", regionResult.error().message());
    return -ENOMEM;
  }
  handle->region = *regionResult;

#ifdef HF3FS_GDR_ENABLED
  cudaIpcMemHandle_t cudaHandle;
  cudaError_t ipcErr = cudaIpcGetMemHandle(&cudaHandle, devicePtr);
  if (ipcErr == cudaSuccess) {
    std::memcpy(handle->ipcHandle.data, &cudaHandle, sizeof(cudaHandle));
    handle->ipcHandle.valid = true;
    XLOGF(DBG,
          "Auto-exported IPC handle for GPU iov: ptr={}, device={}",
          static_cast<const void *>(devicePtr),
          gpu_device_id);
  } else {
    XLOGF(WARN, "Failed to auto-export IPC handle: {} (non-fatal)", cudaGetErrorString(ipcErr));
  }
#endif

  // Generate UUID for this iov
  hf3fs::Uuid uuid = hf3fs::Uuid::random();

  // Initialize the iov structure
  std::memset(iov, 0, sizeof(*iov));
  iov->base = static_cast<uint8_t *>(devicePtr);
  iov->size = size;
  iov->block_size = block_size;
  iov->numa = kGpuIovMagicNuma;  // Magic value to identify GPU iovs
  iov->iovh = handle.get();      // Store handle pointer
  std::memcpy(iov->id, uuid.data, sizeof(iov->id));
  std::strncpy(iov->mount_point, hf3fs_mount_point, sizeof(iov->mount_point) - 1);

  // Register with fuse daemon
  int symlinkResult = createGpuIovSymlink(iov, gpu_device_id, &handle->ipcHandle);
  if (symlinkResult != 0) {
    std::memset(iov, 0, sizeof(*iov));
    return symlinkResult;
  }

  // Track the handle
  registerGpuIov(iov, std::move(handle));

  XLOGF(INFO, "Created GPU iov: ptr={}, size={}, device={}", static_cast<const void *>(devicePtr), size, gpu_device_id);

  return 0;
}

int hf3fs_iovopen_gpu_internal(struct hf3fs_iov *iov,
                               const uint8_t id[16],
                               const char *hf3fs_mount_point,
                               size_t size,
                               size_t block_size,
                               int gpu_device_id) {
  if (!iov || !id || !hf3fs_mount_point || size == 0) {
    return -EINVAL;
  }
  if (block_size != 0) {
    XLOGF(ERR, "GDR iov does not support block_size: {}", block_size);
    return -EINVAL;
  }

  if (!ensureGdrInitialized()) {
    return -ENOTSUP;
  }

  // Validate device ID
  int deviceCount = hf3fs_gdr_device_count();
  if (gpu_device_id < 0 || gpu_device_id >= deviceCount) {
    XLOGF(ERR, "Invalid GPU device ID: {}", gpu_device_id);
    return -ENODEV;
  }

  // Validate mount point length
  if (strlen(hf3fs_mount_point) >= sizeof(iov->mount_point)) {
    return -EINVAL;
  }

  hf3fs::Uuid uuid;
  std::memcpy(uuid.data, id, sizeof(uuid.data));
  XLOGF(DBG, "Opening GPU iov: id={}, size={}, device={}", uuid.toHexString(), size, gpu_device_id);

  // Look up existing iov in fuse namespace
  auto link = fmt::format("{}/3fs-virt/iovs/{}.gdr.d{}", hf3fs_mount_point, uuid.toHexString(), gpu_device_id);

  char target[512];
  ssize_t len = readlink(link.c_str(), target, sizeof(target) - 1);
  if (len < 0) {
    XLOGF(ERR, "GPU iov not found: {}", link);
    return -ENOENT;
  }
  target[len] = '\0';

  auto parsedTarget = hf3fs::lib::parseGdrUri(target);
  if (!parsedTarget) {
    XLOGF(ERR, "Invalid GPU iov target: {}", target);
    return -EINVAL;
  }

  // Verify consistency
  if (parsedTarget->deviceId != gpu_device_id) {
    XLOGF(ERR, "GPU device mismatch: expected {}, got {}", gpu_device_id, parsedTarget->deviceId);
    return -EINVAL;
  }
  if (parsedTarget->size != size) {
    XLOGF(ERR, "GPU size mismatch: expected {}, got {}", size, parsedTarget->size);
    return -EINVAL;
  }

  // Create handle for the existing GPU memory
  auto handle = std::make_unique<GpuIovHandle>();
  handle->deviceId = gpu_device_id;
  handle->ownsMemory = false;  // We don't own it, just opening
  handle->size = parsedTarget->size;

  // Import GPU memory via CUDA IPC handle
#ifdef HF3FS_GDR_ENABLED
  {
    cudaError_t err = cudaSetDevice(gpu_device_id);
    if (err != cudaSuccess) {
      XLOGF(ERR, "cudaSetDevice({}) failed while opening GPU iov: {}", gpu_device_id, cudaGetErrorString(err));
      return -ENODEV;
    }

    cudaIpcMemHandle_t cudaHandle;
    std::memcpy(&cudaHandle, parsedTarget->ipcHandle.data(), sizeof(cudaHandle));
    void *importedPtr = nullptr;
    err = cudaIpcOpenMemHandle(&importedPtr, cudaHandle, cudaIpcMemLazyEnablePeerAccess);
    if (err != cudaSuccess) {
      XLOGF(ERR, "cudaIpcOpenMemHandle failed while opening GPU iov: {}", cudaGetErrorString(err));
      return -EINVAL;
    }
    handle->devicePtr = importedPtr;
    handle->isIpcImported = true;
    std::memcpy(handle->ipcHandle.data, parsedTarget->ipcHandle.data(), hf3fs::lib::kGdrIpcHandleBytes);
    handle->ipcHandle.valid = true;
  }
#else
  XLOGF(ERR, "GPU iov target requires CUDA IPC, but CUDA is disabled in this build");
  return -ENOTSUP;
#endif

  if (!handle->devicePtr) {
    XLOGF(ERR, "GPU iov target resolved to null pointer");
    return -EINVAL;
  }

  // Register with RDMA
  hf3fs::net::AcceleratorMemoryDescriptor desc;
  desc.devicePtr = handle->devicePtr;
  desc.size = parsedTarget->size;
  desc.deviceId = gpu_device_id;
  desc.ipcHandle = handle->ipcHandle;

  auto *cache = hf3fs::net::GDRManager::instance().getRegionCache();
  if (!cache) {
    XLOGF(ERR, "GDR region cache not available");
    return -ENOTSUP;
  }
  auto regionResult = cache->getOrCreate(desc);
  if (!regionResult) {
    XLOGF(ERR, "Failed to register opened GPU memory: {}", regionResult.error().message());
    return -ENOMEM;
  }
  handle->region = *regionResult;

  // Initialize iov
  std::memset(iov, 0, sizeof(*iov));
  iov->base = static_cast<uint8_t *>(handle->devicePtr);
  iov->size = parsedTarget->size;
  iov->block_size = block_size;
  iov->numa = kGpuIovMagicNuma;
  iov->iovh = handle.get();
  std::memcpy(iov->id, id, sizeof(iov->id));
  std::strncpy(iov->mount_point, hf3fs_mount_point, sizeof(iov->mount_point) - 1);

  registerGpuIov(iov, std::move(handle));

  XLOGF(INFO,
        "Opened GPU iov: id={}, ptr={}, size={}",
        uuid.toHexString(),
        static_cast<const void *>(iov->base),
        iov->size);

  return 0;
}

int hf3fs_iovwrap_gpu_internal(struct hf3fs_iov *iov,
                               void *gpu_ptr,
                               const uint8_t id[16],
                               const char *hf3fs_mount_point,
                               size_t size,
                               size_t block_size,
                               int gpu_device_id) {
  if (!iov || !gpu_ptr || !id || !hf3fs_mount_point || size == 0) {
    return -EINVAL;
  }
  if (block_size != 0) {
    XLOGF(ERR, "GDR iov does not support block_size: {}", block_size);
    return -EINVAL;
  }

  if (!ensureGdrInitialized()) {
    return -ENOTSUP;
  }

  // Validate device ID
  int deviceCount = hf3fs_gdr_device_count();
  if (gpu_device_id < 0 || gpu_device_id >= deviceCount) {
    XLOGF(ERR, "Invalid GPU device ID: {} (have {} devices)", gpu_device_id, deviceCount);
    return -ENODEV;
  }

  // Validate mount point
  if (strlen(hf3fs_mount_point) >= sizeof(iov->mount_point)) {
    return -EINVAL;
  }

  XLOGF(DBG,
        "Wrapping GPU memory: ptr={}, size={}, device={}",
        static_cast<const void *>(gpu_ptr),
        size,
        gpu_device_id);

  // Create handle (we don't own the memory)
  auto handle = std::make_unique<GpuIovHandle>();
  handle->deviceId = gpu_device_id;
  handle->devicePtr = gpu_ptr;
  handle->ownsMemory = false;
  handle->size = size;

#ifdef HF3FS_GDR_ENABLED
  cudaError_t ipcSetDeviceErr = cudaSetDevice(gpu_device_id);
  if (ipcSetDeviceErr == cudaSuccess) {
    cudaIpcMemHandle_t cudaHandle;
    cudaError_t ipcErr = cudaIpcGetMemHandle(&cudaHandle, gpu_ptr);
    if (ipcErr == cudaSuccess) {
      std::memcpy(handle->ipcHandle.data, &cudaHandle, sizeof(cudaHandle));
      handle->ipcHandle.valid = true;
    } else {
      XLOGF(WARN, "Failed to auto-export IPC handle for wrapped GPU buffer: {}", cudaGetErrorString(ipcErr));
    }
  } else {
    XLOGF(WARN,
          "cudaSetDevice({}) failed while exporting wrapped GPU buffer IPC handle: {}",
          gpu_device_id,
          cudaGetErrorString(ipcSetDeviceErr));
  }
#endif

  // Create GPU memory descriptor
  hf3fs::net::AcceleratorMemoryDescriptor desc;
  desc.devicePtr = gpu_ptr;
  desc.size = size;
  desc.deviceId = gpu_device_id;
  if (handle->ipcHandle.valid) {
    desc.ipcHandle = handle->ipcHandle;
  }

  // Register with RDMA
  auto *cache = hf3fs::net::GDRManager::instance().getRegionCache();
  if (!cache) {
    XLOGF(ERR, "GDR region cache not available");
    return -ENOTSUP;
  }
  auto regionResult = cache->getOrCreate(desc);
  if (!regionResult) {
    XLOGF(ERR, "Failed to register GPU memory with RDMA: {}", regionResult.error().message());
    return -ENOMEM;
  }
  handle->region = *regionResult;

  // Initialize iov structure
  std::memset(iov, 0, sizeof(*iov));
  iov->base = static_cast<uint8_t *>(gpu_ptr);
  iov->size = size;
  iov->block_size = block_size;
  iov->numa = kGpuIovMagicNuma;
  iov->iovh = handle.get();
  std::memcpy(iov->id, id, sizeof(iov->id));
  std::strncpy(iov->mount_point, hf3fs_mount_point, sizeof(iov->mount_point) - 1);

  // Register with fuse daemon
  int symlinkResult = createGpuIovSymlink(iov, gpu_device_id, &handle->ipcHandle);
  if (symlinkResult != 0) {
    std::memset(iov, 0, sizeof(*iov));
    return symlinkResult;
  }

  // Track handle
  registerGpuIov(iov, std::move(handle));

  XLOGF(INFO,
        "Wrapped GPU memory: ptr={}, size={}, device={}",
        static_cast<const void *>(gpu_ptr),
        size,
        gpu_device_id);

  return 0;
}

void hf3fs_iovunlink_gpu_internal(struct hf3fs_iov *iov) {
  if (!iov) {
    return;
  }

  GpuIovHandle *handle = getGpuHandle(iov);
  if (!handle) {
    XLOGF(WARN, "hf3fs_iovunlink_gpu called on non-GPU iov");
    return;
  }

  // Remove the symlink from fuse namespace
  removeGpuIovSymlink(iov, handle->deviceId);

  XLOGF(DBG, "Unlinked GPU iov: ptr={}, device={}", static_cast<const void *>(iov->base), handle->deviceId);
}

void hf3fs_iovdestroy_gpu_internal(struct hf3fs_iov *iov) {
  if (!iov) {
    return;
  }

  // Unlink first
  hf3fs_iovunlink_gpu_internal(iov);

  // Get and remove the handle
  auto handle = unregisterGpuIov(iov);
  if (!handle) {
    XLOGF(WARN, "hf3fs_iovdestroy_gpu_internal: no handle found");
    return;
  }

  XLOGF(DBG,
        "Destroying GPU iov: ptr={}, size={}, device={}, ownsMemory={}",
        static_cast<const void *>(handle->devicePtr),
        handle->size,
        handle->deviceId,
        handle->ownsMemory);

  // Handle destruction (including potential memory free) happens in destructor
  handle.reset();

  // Clear the iov structure
  std::memset(iov, 0, sizeof(*iov));
}

bool hf3fs_iov_is_gpu_internal(const struct hf3fs_iov *iov) {
  if (!iov) {
    return false;
  }
  return iov->numa == kGpuIovMagicNuma && getGpuHandle(iov) != nullptr;
}

int hf3fs_iov_gpu_device_internal(const struct hf3fs_iov *iov) {
  GpuIovHandle *handle = getGpuHandle(iov);
  return handle ? handle->deviceId : -1;
}

int hf3fs_iovsync_gpu_internal(const struct hf3fs_iov *iov, int direction) {
  if (!iov) {
    return -EINVAL;
  }

  GpuIovHandle *handle = getGpuHandle(iov);
  if (!handle) {
    return -EINVAL;
  }

  XLOGF(DBG,
        "GPU sync: ptr={}, size={}, device={}, direction={}",
        static_cast<const void *>(handle->devicePtr),
        handle->size,
        handle->deviceId,
        direction);

#ifdef HF3FS_GDR_ENABLED
  cudaError_t err = cudaSetDevice(handle->deviceId);
  if (err != cudaSuccess) {
    XLOGF(ERR, "cudaSetDevice({}) failed: {}", handle->deviceId, cudaGetErrorString(err));
    return -ENODEV;
  }
  err = cudaDeviceSynchronize();
  if (err != cudaSuccess) {
    XLOGF(ERR, "cudaDeviceSynchronize failed: {}", cudaGetErrorString(err));
    return -EIO;
  }
#else
  (void)direction;
#endif

  // For most cases, nvidia_peermem handles coherency automatically
  return 0;
}

}  // extern "C"
