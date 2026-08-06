#include "lib/common/CudaIpcMemory.h"

#include <cstdint>
#include <cstring>
#include <folly/logging/xlog.h>
#include <limits>
#include <utility>

#ifdef HF3FS_ENABLE_GDR
#include <cuda_runtime.h>
#endif

#include "common/cuda/CudaMemory.h"

namespace hf3fs::lib {

Result<CudaIpcExport> exportCudaIpcMemory(void *devicePtr, size_t viewSize, int deviceId) {
#ifdef HF3FS_ENABLE_GDR
  static_assert(sizeof(cudaIpcMemHandle_t) == kCudaIpcHandleBytes);

  if (!devicePtr || viewSize == 0 || deviceId < 0) {
    return makeError(StatusCode::kInvalidArg, "invalid CUDA IPC export parameters");
  }

  auto available = cuda::supportsIpc(deviceId);
  RETURN_ON_ERROR(available);
  if (!*available) {
    return MAKE_ERROR_F(StatusCode::kInvalidArg, "CUDA device {} does not support IPC", deviceId);
  }

  auto view = cuda::prepareGdrMemory(devicePtr, viewSize, deviceId);
  RETURN_ON_ERROR(view);
  auto guard = cuda::ScopedDevice::create(deviceId);
  RETURN_ON_ERROR(guard);

  cudaIpcMemHandle_t runtimeHandle;
  auto runtimeResult = cudaIpcGetMemHandle(&runtimeHandle, view->allocationBase);
  if (runtimeResult != cudaSuccess) {
    return MAKE_ERROR_F(StatusCode::kIOError, "cudaIpcGetMemHandle failed: {}", cudaGetErrorString(runtimeResult));
  }

  CudaIpcExport result;
  result.allocationBase = view->allocationBase;
  result.allocationSize = view->allocationSize;
  result.offset = view->offset;
  std::memcpy(result.ipcHandle.data(), &runtimeHandle, sizeof(runtimeHandle));
  return result;
#else
  (void)devicePtr;
  (void)viewSize;
  (void)deviceId;
  return makeError(StatusCode::kNotImplemented, "CUDA IPC is disabled in this build");
#endif
}

Result<CudaIpcMapping> CudaIpcMapping::open(int deviceId, const CudaIpcHandle &ipcHandle, size_t allocationSize) {
#ifdef HF3FS_ENABLE_GDR
  static_assert(sizeof(cudaIpcMemHandle_t) == kCudaIpcHandleBytes);

  if (deviceId < 0 || allocationSize == 0) {
    return makeError(StatusCode::kInvalidArg, "invalid CUDA IPC import parameters");
  }
  auto guard = cuda::ScopedDevice::create(deviceId);
  RETURN_ON_ERROR(guard);

  cudaIpcMemHandle_t runtimeHandle;
  std::memcpy(&runtimeHandle, ipcHandle.data(), sizeof(runtimeHandle));

  void *importedBase = nullptr;
  auto runtimeResult = cudaIpcOpenMemHandle(&importedBase, runtimeHandle, cudaIpcMemLazyEnablePeerAccess);
  if (runtimeResult != cudaSuccess) {
    return MAKE_ERROR_F(StatusCode::kIOError, "cudaIpcOpenMemHandle failed: {}", cudaGetErrorString(runtimeResult));
  }

  CudaIpcMapping mapping(deviceId, importedBase, allocationSize);
  auto view = cuda::inspectDeviceMemory(importedBase, allocationSize, deviceId);
  RETURN_ON_ERROR(view);
  if (view->allocationBase != importedBase || view->allocationSize != allocationSize || view->offset != 0) {
    return MAKE_ERROR_F(StatusCode::kInvalidArg,
                        "imported CUDA allocation range mismatch: URI size {}, actual size {}",
                        allocationSize,
                        view->allocationSize);
  }

  return Result<CudaIpcMapping>(std::move(mapping));
#else
  (void)deviceId;
  (void)ipcHandle;
  (void)allocationSize;
  return makeError(StatusCode::kNotImplemented, "CUDA IPC is disabled in this build");
#endif
}

CudaIpcMapping::CudaIpcMapping(CudaIpcMapping &&other) noexcept
    : deviceId_(std::exchange(other.deviceId_, -1)),
      allocationBase_(std::exchange(other.allocationBase_, nullptr)),
      allocationSize_(std::exchange(other.allocationSize_, 0)) {}

CudaIpcMapping &CudaIpcMapping::operator=(CudaIpcMapping &&other) noexcept {
  if (this != &other) {
    reset();
    deviceId_ = std::exchange(other.deviceId_, -1);
    allocationBase_ = std::exchange(other.allocationBase_, nullptr);
    allocationSize_ = std::exchange(other.allocationSize_, 0);
  }
  return *this;
}

CudaIpcMapping::~CudaIpcMapping() { reset(); }

Result<void *> CudaIpcMapping::view(size_t offset, size_t size) const {
  if (!allocationBase_ || size == 0 || offset > allocationSize_ || size > allocationSize_ - offset) {
    return makeError(StatusCode::kInvalidArg, "CUDA IPC view is outside the imported allocation");
  }
  auto address = reinterpret_cast<uintptr_t>(allocationBase_);
  if (address > std::numeric_limits<uintptr_t>::max() - offset) {
    return makeError(StatusCode::kInvalidArg, "CUDA IPC view address overflows uintptr_t");
  }
  return reinterpret_cast<void *>(address + offset);
}

void CudaIpcMapping::reset() noexcept {
  if (!allocationBase_) {
    return;
  }

  void *base = std::exchange(allocationBase_, nullptr);
  allocationSize_ = 0;
#ifdef HF3FS_ENABLE_GDR
  auto guard = cuda::ScopedDevice::create(deviceId_);
  if (!guard) {
    XLOGF(WARN, "Failed to select CUDA device {} before closing IPC mapping: {}", deviceId_, guard.error());
  }
  auto error = cudaIpcCloseMemHandle(base);
  if (error != cudaSuccess) {
    XLOGF(WARN, "cudaIpcCloseMemHandle failed: {}", cudaGetErrorString(error));
  }
#else
  (void)base;
#endif
  deviceId_ = -1;
}

}  // namespace hf3fs::lib
