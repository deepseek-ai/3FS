/**
 * GPU Direct RDMA (GDR) user API implementation.
 *
 * The application process owns only CUDA allocation/IPC publication. GPU
 * memory registration is performed by the FUSE process when the publication
 * symlink is resolved.
 */

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <unistd.h>
#include <unordered_map>

#include "UsrbIoGdrInternal.h"
#include "hf3fs_usrbio.h"

#ifdef HF3FS_ENABLE_GDR
#include <cuda_runtime.h>
#endif

#include "common/cuda/CudaMemory.h"
#include "common/utils/Uuid.h"
#include "lib/common/CudaIpcMemory.h"
#include "lib/common/GdrUri.h"

namespace {

static_assert(hf3fs::lib::kCudaIpcHandleBytes == hf3fs::lib::kGdrIpcHandleBytes);

struct GpuIovHandle {
  int deviceId = -1;

  void *allocationBase = nullptr;
  size_t allocationSize = 0;
  void *viewPtr = nullptr;
  size_t viewOffset = 0;
  size_t viewSize = 0;

  bool ownsMemory = false;
  bool ownsPublication = false;
  std::unique_ptr<hf3fs::lib::CudaIpcMapping> importedMapping;
  hf3fs::lib::CudaIpcHandle ipcHandle{};

  ~GpuIovHandle() {
    importedMapping.reset();
    if (ownsMemory && allocationBase) {
#ifdef HF3FS_ENABLE_GDR
      auto guard = hf3fs::cuda::ScopedDevice::create(deviceId);
      if (!guard) {
        XLOGF(WARN, "Failed to select CUDA device {} before freeing GPU iov: {}", deviceId, guard.error());
        return;
      }
      auto error = cudaFree(allocationBase);
      if (error != cudaSuccess) {
        XLOGF(WARN, "cudaFree failed for GPU iov: {}", cudaGetErrorString(error));
      }
#endif
    }
  }
};

std::mutex gGpuIovMutex;
std::unordered_map<void *, std::unique_ptr<GpuIovHandle>> gGpuIovHandles;

int resultToErrno(const hf3fs::Status &status) {
  switch (status.code()) {
    case hf3fs::StatusCode::kInvalidArg:
      return -EINVAL;
    case hf3fs::StatusCode::kNotImplemented:
      return -ENOTSUP;
    case hf3fs::StatusCode::kNotEnoughMemory:
      return -ENOMEM;
    default:
      return -EIO;
  }
}

std::string gpuIovLink(const struct hf3fs_iov &iov, int deviceId) {
  hf3fs::Uuid uuid;
  std::memcpy(uuid.data, iov.id, sizeof(uuid.data));
  return fmt::format("{}/3fs-virt/iovs/{}.gdr.d{}", iov.mount_point, uuid.toHexString(), deviceId);
}

int createGpuIovSymlink(const struct hf3fs_iov &iov, const GpuIovHandle &handle) {
  auto target = hf3fs::lib::formatGdrUri(handle.deviceId,
                                         handle.allocationSize,
                                         handle.viewOffset,
                                         handle.viewSize,
                                         handle.ipcHandle.data(),
                                         handle.ipcHandle.size());
  if (target.empty()) {
    return -EINVAL;
  }

  auto link = gpuIovLink(iov, handle.deviceId);
  if (symlink(target.c_str(), link.c_str()) < 0) {
    auto error = errno;
    XLOGF(WARN, "Failed to publish GPU iov {} -> {}: {}", link, target, strerror(error));
    return -error;
  }

  XLOGF(DBG, "Published GPU iov {} -> {}", link, target);
  return 0;
}

int removeGpuIovSymlink(const struct hf3fs_iov &iov, int deviceId) {
  auto link = gpuIovLink(iov, deviceId);
  if (unlink(link.c_str()) == 0) {
    return 0;
  }

  auto error = errno;
  if (error != ENOENT) {
    XLOGF(WARN, "Failed to unlink GPU iov {}: {}", link, strerror(error));
  }
  return -error;
}

struct GpuIovSnapshot {
  int deviceId;
  bool ownsPublication;
};

std::optional<GpuIovSnapshot> getGpuHandleSnapshot(const struct hf3fs_iov *iov) {
  if (!iov || !iov->iovh) {
    return std::nullopt;
  }
  std::lock_guard lock(gGpuIovMutex);
  auto it = gGpuIovHandles.find(iov->iovh);
  if (it == gGpuIovHandles.end()) {
    return std::nullopt;
  }
  return GpuIovSnapshot{it->second->deviceId, it->second->ownsPublication};
}

std::unique_ptr<GpuIovHandle> unregisterGpuIov(const struct hf3fs_iov *iov) {
  if (!iov || !iov->iovh) {
    return nullptr;
  }
  std::lock_guard lock(gGpuIovMutex);
  auto it = gGpuIovHandles.find(iov->iovh);
  if (it == gGpuIovHandles.end()) {
    return nullptr;
  }
  auto handle = std::move(it->second);
  gGpuIovHandles.erase(it);
  return handle;
}

int finalizeGpuIov(struct hf3fs_iov *iov,
                   std::unique_ptr<GpuIovHandle> handle,
                   const uint8_t id[16],
                   const char *mountPoint,
                   size_t blockSize) {
  if (!iov || !handle || !id || !mountPoint || std::strlen(mountPoint) >= sizeof(iov->mount_point)) {
    return -EINVAL;
  }

  struct hf3fs_iov result{};
  result.base = static_cast<uint8_t *>(handle->viewPtr);
  result.iovh = handle.get();
  std::memcpy(result.id, id, sizeof(result.id));
  std::strcpy(result.mount_point, mountPoint);
  result.size = handle->viewSize;
  result.block_size = blockSize;
  result.numa = -1;

  if (handle->ownsPublication) {
    auto aliveResult = hf3fs_ensure_iov_mount_fd_internal(mountPoint);
    if (aliveResult != 0) {
      XLOGF(ERR, "Failed to hold iovs directory for mount {}: {}", mountPoint, strerror(-aliveResult));
      return aliveResult;
    }
  }

  auto *key = handle.get();
  try {
    std::lock_guard lock(gGpuIovMutex);
    auto [it, inserted] = gGpuIovHandles.try_emplace(key);
    if (!inserted) {
      return -EEXIST;
    }
    it->second = std::move(handle);
  } catch (const std::bad_alloc &) {
    return -ENOMEM;
  }

  if (key->ownsPublication) {
    auto publishResult = createGpuIovSymlink(result, *key);
    if (publishResult != 0) {
      auto rollback = unregisterGpuIov(&result);
      XLOGF_IF(FATAL, !rollback, "GPU iov handle disappeared during publication rollback");
      return publishResult;
    }
  }

  *iov = result;
  return 0;
}

int registerAndPublishGpuIov(struct hf3fs_iov *iov,
                             std::unique_ptr<GpuIovHandle> handle,
                             const uint8_t id[16],
                             const char *mountPoint,
                             size_t blockSize) {
  handle->ownsPublication = true;
  return finalizeGpuIov(iov, std::move(handle), id, mountPoint, blockSize);
}

int validateGpuDevice(int deviceId) {
  auto available = hf3fs::cuda::supportsIpc(deviceId);
  if (!available) {
    XLOGF(ERR, "Failed to query CUDA device {}: {}", deviceId, available.error());
    return resultToErrno(available.error());
  }
  return *available ? 0 : -ENODEV;
}

int allocateGpuMemory(size_t size, int deviceId, void **devicePtr) {
  if (!devicePtr || size == 0) {
    return -EINVAL;
  }
#ifdef HF3FS_ENABLE_GDR
  auto guard = hf3fs::cuda::ScopedDevice::create(deviceId);
  if (!guard) {
    XLOGF(ERR, "Failed to select CUDA device {}: {}", deviceId, guard.error());
    return -ENODEV;
  }
  auto error = cudaMalloc(devicePtr, size);
  if (error != cudaSuccess) {
    XLOGF(ERR, "cudaMalloc({}) failed: {}", size, cudaGetErrorString(error));
    return error == cudaErrorMemoryAllocation ? -ENOMEM : -EIO;
  }
  return 0;
#else
  (void)deviceId;
  *devicePtr = nullptr;
  return -ENOTSUP;
#endif
}

}  // namespace

extern "C" {

bool hf3fs_gdr_available(void) {
  auto count = hf3fs::cuda::deviceCount();
  if (!count) {
    return false;
  }
  for (int deviceId = 0; deviceId < *count; ++deviceId) {
    auto available = hf3fs::cuda::supportsIpc(deviceId);
    if (available && *available) {
      return true;
    }
  }
  return false;
}

int hf3fs_gdr_device_count(void) {
  auto count = hf3fs::cuda::deviceCount();
  return count ? *count : 0;
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
    return -EINVAL;
  }
  auto deviceResult = validateGpuDevice(gpu_device_id);
  if (deviceResult != 0) {
    return deviceResult;
  }

  void *devicePtr = nullptr;
  auto allocationResult = allocateGpuMemory(size, gpu_device_id, &devicePtr);
  if (allocationResult != 0) {
    return allocationResult;
  }

  auto handle = std::make_unique<GpuIovHandle>();
  handle->deviceId = gpu_device_id;
  handle->allocationBase = devicePtr;
  handle->allocationSize = size;
  handle->viewPtr = devicePtr;
  handle->viewSize = size;
  handle->ownsMemory = true;

  auto ipcExport = hf3fs::lib::exportCudaIpcMemory(devicePtr, size, gpu_device_id);
  if (!ipcExport) {
    XLOGF(ERR, "Failed to export allocated GPU iov: {}", ipcExport.error());
    return resultToErrno(ipcExport.error());
  }
  if (ipcExport->allocationBase != devicePtr || ipcExport->offset != 0) {
    XLOGF(ERR, "cudaMalloc returned a pointer that is not the CUDA allocation base");
    return -EIO;
  }
  handle->allocationBase = ipcExport->allocationBase;
  handle->allocationSize = ipcExport->allocationSize;
  handle->viewOffset = ipcExport->offset;
  handle->ipcHandle = ipcExport->ipcHandle;

  auto uuid = hf3fs::Uuid::random();
  return registerAndPublishGpuIov(iov,
                                  std::move(handle),
                                  reinterpret_cast<const uint8_t *>(uuid.data),
                                  hf3fs_mount_point,
                                  block_size);
}

int hf3fs_iovopen_gpu_internal(struct hf3fs_iov *iov,
                               const uint8_t id[16],
                               const char *hf3fs_mount_point,
                               size_t size,
                               size_t block_size,
                               int gpu_device_id) {
  if (!iov || !id || !hf3fs_mount_point || size == 0 || std::strlen(hf3fs_mount_point) >= sizeof(iov->mount_point)) {
    return -EINVAL;
  }
  if (block_size != 0) {
    return -EINVAL;
  }
  auto deviceResult = validateGpuDevice(gpu_device_id);
  if (deviceResult != 0) {
    return deviceResult;
  }

  hf3fs::Uuid uuid;
  std::memcpy(uuid.data, id, sizeof(uuid.data));
  auto link = fmt::format("{}/3fs-virt/iovs/{}.gdr.d{}", hf3fs_mount_point, uuid.toHexString(), gpu_device_id);

  char target[512];
  auto length = readlink(link.c_str(), target, sizeof(target) - 1);
  if (length < 0) {
    return -errno;
  }
  target[length] = '\0';

  auto parsed = hf3fs::lib::parseGdrUri(target);
  if (!parsed || parsed->deviceId != gpu_device_id || parsed->size != size) {
    return -EINVAL;
  }

  hf3fs::lib::CudaIpcHandle ipcHandle;
  std::copy(parsed->ipcHandle.begin(), parsed->ipcHandle.end(), ipcHandle.begin());
  auto mapping = hf3fs::lib::CudaIpcMapping::open(gpu_device_id, ipcHandle, parsed->allocationSize);
  if (!mapping) {
    XLOGF(ERR, "Failed to import GPU iov {}: {}", uuid.toHexString(), mapping.error());
    return resultToErrno(mapping.error());
  }
  auto view = mapping->view(parsed->offset, parsed->size);
  if (!view) {
    return resultToErrno(view.error());
  }

  auto handle = std::make_unique<GpuIovHandle>();
  handle->deviceId = gpu_device_id;
  handle->allocationBase = mapping->allocationBase();
  handle->allocationSize = parsed->allocationSize;
  handle->viewPtr = *view;
  handle->viewOffset = parsed->offset;
  handle->viewSize = parsed->size;
  handle->ipcHandle = ipcHandle;
  handle->importedMapping = std::make_unique<hf3fs::lib::CudaIpcMapping>(std::move(*mapping));

  return finalizeGpuIov(iov, std::move(handle), id, hf3fs_mount_point, block_size);
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
    return -EINVAL;
  }
  auto deviceResult = validateGpuDevice(gpu_device_id);
  if (deviceResult != 0) {
    return deviceResult;
  }

  auto ipcExport = hf3fs::lib::exportCudaIpcMemory(gpu_ptr, size, gpu_device_id);
  if (!ipcExport) {
    XLOGF(ERR, "Failed to export wrapped GPU iov: {}", ipcExport.error());
    return resultToErrno(ipcExport.error());
  }

  auto handle = std::make_unique<GpuIovHandle>();
  handle->deviceId = gpu_device_id;
  handle->allocationBase = ipcExport->allocationBase;
  handle->allocationSize = ipcExport->allocationSize;
  handle->viewPtr = gpu_ptr;
  handle->viewOffset = ipcExport->offset;
  handle->viewSize = size;
  handle->ipcHandle = ipcExport->ipcHandle;

  return registerAndPublishGpuIov(iov, std::move(handle), id, hf3fs_mount_point, block_size);
}

int hf3fs_iovunlink_gpu_internal(struct hf3fs_iov *iov) {
  if (!iov || !iov->iovh) {
    return 0;
  }

  std::lock_guard lock(gGpuIovMutex);
  auto it = gGpuIovHandles.find(iov->iovh);
  if (it == gGpuIovHandles.end() || !it->second->ownsPublication) {
    return 0;
  }

  auto result = removeGpuIovSymlink(*iov, it->second->deviceId);
  if (result == 0 || result == -ENOENT) {
    it->second->ownsPublication = false;
    return 0;
  }
  return result;
}

int hf3fs_iovunlink_gpu_publication_internal(const struct hf3fs_iov *iov, int device_id, bool owns_publication) {
  if (!iov || !owns_publication) {
    return 0;
  }
  return removeGpuIovSymlink(*iov, device_id);
}

void hf3fs_iovdestroy_gpu_internal(struct hf3fs_iov *iov) {
  if (!iov || !iov->iovh) {
    return;
  }

  std::unique_ptr<GpuIovHandle> handle;
  {
    std::lock_guard lock(gGpuIovMutex);
    auto it = gGpuIovHandles.find(iov->iovh);
    if (it == gGpuIovHandles.end()) {
      return;
    }

    if (it->second->ownsPublication) {
      auto unlinkResult = removeGpuIovSymlink(*iov, it->second->deviceId);
      if (unlinkResult != 0 && unlinkResult != -ENOENT) {
        XLOGF(ERR, "GPU iov destroy retained handle after publication unlink failed: {}", strerror(-unlinkResult));
        return;
      }
      it->second->ownsPublication = false;
    }

    handle = std::move(it->second);
    gGpuIovHandles.erase(it);
  }

  handle.reset();
  std::memset(iov, 0, sizeof(*iov));
}

bool hf3fs_iov_is_gpu_internal(const struct hf3fs_iov *iov) { return getGpuHandleSnapshot(iov).has_value(); }

int hf3fs_iov_gpu_device_internal(const struct hf3fs_iov *iov) {
  auto handle = getGpuHandleSnapshot(iov);
  return handle ? handle->deviceId : -1;
}

int hf3fs_iovsync_gpu_internal(const struct hf3fs_iov *iov, int direction) {
  auto handle = getGpuHandleSnapshot(iov);
  if (!handle) {
    return -EINVAL;
  }
#ifdef HF3FS_ENABLE_GDR
  auto guard = hf3fs::cuda::ScopedDevice::create(handle->deviceId);
  if (!guard) {
    return -ENODEV;
  }
  auto error = cudaDeviceSynchronize();
  if (error != cudaSuccess) {
    XLOGF(ERR, "cudaDeviceSynchronize failed: {}", cudaGetErrorString(error));
    return -EIO;
  }
#else
  (void)direction;
  return -ENOTSUP;
#endif
  (void)direction;
  return 0;
}

}  // extern "C"
