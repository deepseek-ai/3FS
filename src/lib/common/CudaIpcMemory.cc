#include "lib/common/CudaIpcMemory.h"

#include <cstring>
#include <fmt/format.h>
#include <folly/logging/xlog.h>
#include <string>
#include <utility>

#ifdef HF3FS_GDR_ENABLED
#include <cuda.h>
#include <cuda_runtime.h>
#endif

namespace hf3fs::lib {
namespace {

#ifdef HF3FS_GDR_ENABLED
Result<Void> setCudaDevice(int deviceId) {
  auto error = cudaSetDevice(deviceId);
  if (error != cudaSuccess) {
    return makeError(StatusCode::kIOError, "cudaSetDevice({}) failed: {}", deviceId, cudaGetErrorString(error));
  }
  return Void{};
}

std::string driverError(CUresult result) {
  const char *name = nullptr;
  const char *message = nullptr;
  cuGetErrorName(result, &name);
  cuGetErrorString(result, &message);
  return fmt::format("{}: {}", name ? name : "CUDA_ERROR_UNKNOWN", message ? message : "unknown error");
}
#endif

}  // namespace

Result<int> cudaIpcDeviceCount() {
#ifdef HF3FS_GDR_ENABLED
  int count = 0;
  auto error = cudaGetDeviceCount(&count);
  if (error == cudaErrorNoDevice) {
    cudaGetLastError();
    return 0;
  }
  if (error != cudaSuccess) {
    return makeError(StatusCode::kIOError, "cudaGetDeviceCount failed: {}", cudaGetErrorString(error));
  }
  return count;
#else
  return 0;
#endif
}

Result<bool> cudaIpcDeviceAvailable(int deviceId) {
#ifdef HF3FS_GDR_ENABLED
  auto count = cudaIpcDeviceCount();
  RETURN_ON_ERROR(count);
  if (deviceId < 0 || deviceId >= *count) {
    return false;
  }

  int unifiedAddressing = 0;
  auto error = cudaDeviceGetAttribute(&unifiedAddressing, cudaDevAttrUnifiedAddressing, deviceId);
  if (error != cudaSuccess) {
    return makeError(StatusCode::kIOError,
                     "failed to query unified addressing for CUDA device {}: {}",
                     deviceId,
                     cudaGetErrorString(error));
  }

  int computeMode = cudaComputeModeProhibited;
  error = cudaDeviceGetAttribute(&computeMode, cudaDevAttrComputeMode, deviceId);
  if (error != cudaSuccess) {
    return makeError(StatusCode::kIOError,
                     "failed to query compute mode for CUDA device {}: {}",
                     deviceId,
                     cudaGetErrorString(error));
  }

  return unifiedAddressing != 0 && computeMode != cudaComputeModeProhibited;
#else
  (void)deviceId;
  return false;
#endif
}

Result<CudaIpcExport> exportCudaIpcMemory(void *devicePtr, size_t viewSize, int deviceId) {
#ifdef HF3FS_GDR_ENABLED
  static_assert(sizeof(cudaIpcMemHandle_t) == kCudaIpcHandleBytes);

  if (!devicePtr || viewSize == 0 || deviceId < 0) {
    return makeError(StatusCode::kInvalidArg, "invalid CUDA IPC export parameters");
  }

  auto available = cudaIpcDeviceAvailable(deviceId);
  RETURN_ON_ERROR(available);
  if (!*available) {
    return makeError(StatusCode::kInvalidArg, "CUDA device {} does not support IPC", deviceId);
  }
  RETURN_ON_ERROR(setCudaDevice(deviceId));

  auto address = reinterpret_cast<CUdeviceptr>(devicePtr);
  CUdevice pointerDevice = -1;
  auto driverResult = cuPointerGetAttribute(&pointerDevice, CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL, address);
  if (driverResult != CUDA_SUCCESS) {
    return makeError(StatusCode::kInvalidArg, "failed to query CUDA pointer device: {}", driverError(driverResult));
  }
  if (pointerDevice != deviceId) {
    return makeError(StatusCode::kInvalidArg,
                     "CUDA pointer belongs to device {}, expected {}",
                     pointerDevice,
                     deviceId);
  }

  CUdeviceptr allocationBase = 0;
  size_t allocationSize = 0;
  driverResult = cuMemGetAddressRange(&allocationBase, &allocationSize, address);
  if (driverResult != CUDA_SUCCESS) {
    return makeError(StatusCode::kInvalidArg,
                     "cuMemGetAddressRange failed for CUDA pointer: {}",
                     driverError(driverResult));
  }
  if (address < allocationBase) {
    return makeError(StatusCode::kInvalidArg, "CUDA allocation range does not contain the pointer");
  }

  const auto offset = static_cast<size_t>(address - allocationBase);
  if (allocationSize == 0 || offset > allocationSize || viewSize > allocationSize - offset) {
    return makeError(StatusCode::kInvalidArg,
                     "CUDA view [{}, {}) exceeds allocation size {}",
                     offset,
                     offset + viewSize,
                     allocationSize);
  }

  cudaIpcMemHandle_t runtimeHandle;
  auto runtimeResult = cudaIpcGetMemHandle(&runtimeHandle, reinterpret_cast<void *>(allocationBase));
  if (runtimeResult != cudaSuccess) {
    return makeError(StatusCode::kIOError, "cudaIpcGetMemHandle failed: {}", cudaGetErrorString(runtimeResult));
  }

  CudaIpcExport result;
  result.allocationBase = reinterpret_cast<void *>(allocationBase);
  result.allocationSize = allocationSize;
  result.offset = offset;
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
#ifdef HF3FS_GDR_ENABLED
  static_assert(sizeof(cudaIpcMemHandle_t) == kCudaIpcHandleBytes);

  if (deviceId < 0 || allocationSize == 0) {
    return makeError(StatusCode::kInvalidArg, "invalid CUDA IPC import parameters");
  }
  RETURN_ON_ERROR(setCudaDevice(deviceId));

  cudaIpcMemHandle_t runtimeHandle;
  std::memcpy(&runtimeHandle, ipcHandle.data(), sizeof(runtimeHandle));

  void *importedBase = nullptr;
  auto runtimeResult = cudaIpcOpenMemHandle(&importedBase, runtimeHandle, cudaIpcMemLazyEnablePeerAccess);
  if (runtimeResult != cudaSuccess) {
    return makeError(StatusCode::kIOError, "cudaIpcOpenMemHandle failed: {}", cudaGetErrorString(runtimeResult));
  }

  CudaIpcMapping mapping(deviceId, importedBase, allocationSize);
  CUdeviceptr actualBase = 0;
  size_t actualSize = 0;
  auto driverResult = cuMemGetAddressRange(&actualBase, &actualSize, reinterpret_cast<CUdeviceptr>(importedBase));
  if (driverResult != CUDA_SUCCESS) {
    return makeError(StatusCode::kIOError,
                     "cuMemGetAddressRange failed for imported CUDA allocation: {}",
                     driverError(driverResult));
  }
  if (reinterpret_cast<void *>(actualBase) != importedBase || actualSize != allocationSize) {
    return makeError(StatusCode::kInvalidArg,
                     "imported CUDA allocation range mismatch: URI size {}, actual size {}",
                     allocationSize,
                     actualSize);
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
  return static_cast<void *>(static_cast<uint8_t *>(allocationBase_) + offset);
}

void CudaIpcMapping::reset() noexcept {
  if (!allocationBase_) {
    return;
  }

  void *base = std::exchange(allocationBase_, nullptr);
  allocationSize_ = 0;
#ifdef HF3FS_GDR_ENABLED
  auto error = cudaSetDevice(deviceId_);
  if (error != cudaSuccess) {
    XLOGF(WARN, "cudaSetDevice({}) failed before closing IPC mapping: {}", deviceId_, cudaGetErrorString(error));
  }
  error = cudaIpcCloseMemHandle(base);
  if (error != cudaSuccess) {
    XLOGF(WARN, "cudaIpcCloseMemHandle failed: {}", cudaGetErrorString(error));
  }
#else
  (void)base;
#endif
  deviceId_ = -1;
}

}  // namespace hf3fs::lib
