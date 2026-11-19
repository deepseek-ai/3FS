#include "common/cuda/CudaMemory.h"

#include <cstdint>
#include <fmt/format.h>
#include <folly/logging/xlog.h>
#include <string>
#include <utility>

#ifdef HF3FS_ENABLE_GDR
#include <cuda.h>
#include <cuda_runtime.h>
#endif

namespace hf3fs::cuda {
namespace {

#ifdef HF3FS_ENABLE_GDR
std::string driverError(CUresult result) {
  const char *name = nullptr;
  const char *message = nullptr;
  cuGetErrorName(result, &name);
  cuGetErrorString(result, &message);
  return fmt::format("{}: {}", name ? name : "CUDA_ERROR_UNKNOWN", message ? message : "unknown error");
}
#endif

}  // namespace

ScopedDevice::ScopedDevice(ScopedDevice &&other) noexcept
    : previousDevice_(std::exchange(other.previousDevice_, -1)),
      restore_(std::exchange(other.restore_, false)) {}

ScopedDevice &ScopedDevice::operator=(ScopedDevice &&other) noexcept {
  if (this != &other) {
    restore();
    previousDevice_ = std::exchange(other.previousDevice_, -1);
    restore_ = std::exchange(other.restore_, false);
  }
  return *this;
}

ScopedDevice::~ScopedDevice() { restore(); }

void ScopedDevice::restore() noexcept {
#ifdef HF3FS_ENABLE_GDR
  if (restore_) {
    restore_ = false;
    auto error = cudaSetDevice(previousDevice_);
    XLOGF_IF(WARN,
             error != cudaSuccess,
             "cudaSetDevice({}) failed while restoring CUDA device: {}",
             previousDevice_,
             cudaGetErrorString(error));
  }
#endif
}

Result<ScopedDevice> ScopedDevice::create(int deviceId) {
#ifdef HF3FS_ENABLE_GDR
  if (deviceId < 0) {
    return MAKE_ERROR_F(StatusCode::kInvalidArg, "invalid CUDA device {}", deviceId);
  }

  int previousDevice = -1;
  auto error = cudaGetDevice(&previousDevice);
  if (error != cudaSuccess) {
    return MAKE_ERROR_F(StatusCode::kIOError, "cudaGetDevice failed: {}", cudaGetErrorString(error));
  }
  if (previousDevice == deviceId) {
    return ScopedDevice(previousDevice, false);
  }

  error = cudaSetDevice(deviceId);
  if (error != cudaSuccess) {
    return MAKE_ERROR_F(StatusCode::kIOError, "cudaSetDevice({}) failed: {}", deviceId, cudaGetErrorString(error));
  }
  return ScopedDevice(previousDevice, true);
#else
  (void)deviceId;
  return makeError(StatusCode::kNotImplemented, "CUDA/GDR is disabled in this build");
#endif
}

Result<int> deviceCount() {
#ifdef HF3FS_ENABLE_GDR
  int count = 0;
  auto error = cudaGetDeviceCount(&count);
  if (error == cudaErrorNoDevice) {
    cudaGetLastError();
    return 0;
  }
  if (error != cudaSuccess) {
    return MAKE_ERROR_F(StatusCode::kIOError, "cudaGetDeviceCount failed: {}", cudaGetErrorString(error));
  }
  return count;
#else
  return 0;
#endif
}

Result<bool> supportsIpc(int deviceId) {
#ifdef HF3FS_ENABLE_GDR
  auto count = deviceCount();
  RETURN_ON_ERROR(count);
  if (deviceId < 0 || deviceId >= *count) {
    return false;
  }

  int unifiedAddressing = 0;
  auto error = cudaDeviceGetAttribute(&unifiedAddressing, cudaDevAttrUnifiedAddressing, deviceId);
  if (error != cudaSuccess) {
    return MAKE_ERROR_F(StatusCode::kIOError,
                        "failed to query unified addressing for CUDA device {}: {}",
                        deviceId,
                        cudaGetErrorString(error));
  }

  int computeMode = cudaComputeModeProhibited;
  error = cudaDeviceGetAttribute(&computeMode, cudaDevAttrComputeMode, deviceId);
  if (error != cudaSuccess) {
    return MAKE_ERROR_F(StatusCode::kIOError,
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

Result<DeviceMemoryView> inspectDeviceMemory(void *devicePtr, size_t viewSize, int expectedDeviceId) {
#ifdef HF3FS_ENABLE_GDR
  if (!devicePtr || viewSize == 0 || expectedDeviceId < -1) {
    return makeError(StatusCode::kInvalidArg, "invalid CUDA device-memory view");
  }

  int deviceId = expectedDeviceId;
  if (deviceId < 0) {
    cudaPointerAttributes attributes;
    auto runtimeResult = cudaPointerGetAttributes(&attributes, devicePtr);
    if (runtimeResult != cudaSuccess) {
      auto message = std::string(cudaGetErrorString(runtimeResult));
      cudaGetLastError();
      return MAKE_ERROR_F(StatusCode::kInvalidArg, "failed to inspect CUDA pointer: {}", message);
    }
    if (attributes.type != cudaMemoryTypeDevice || attributes.device < 0) {
      return makeError(StatusCode::kInvalidArg, "pointer is not CUDA device memory");
    }
    deviceId = attributes.device;
  }

  auto guard = ScopedDevice::create(deviceId);
  RETURN_ON_ERROR(guard);

  auto address = reinterpret_cast<CUdeviceptr>(devicePtr);
  CUmemorytype memoryType = CU_MEMORYTYPE_HOST;
  auto driverResult = cuPointerGetAttribute(&memoryType, CU_POINTER_ATTRIBUTE_MEMORY_TYPE, address);
  if (driverResult != CUDA_SUCCESS || memoryType != CU_MEMORYTYPE_DEVICE) {
    if (driverResult != CUDA_SUCCESS) {
      return MAKE_ERROR_F(StatusCode::kInvalidArg,
                          "failed to inspect CUDA pointer {}: {}",
                          fmt::ptr(devicePtr),
                          driverError(driverResult));
    }
    return MAKE_ERROR_F(StatusCode::kInvalidArg, "pointer {} is not CUDA device memory", fmt::ptr(devicePtr));
  }

  CUdevice pointerDevice = -1;
  driverResult = cuPointerGetAttribute(&pointerDevice, CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL, address);
  if (driverResult != CUDA_SUCCESS) {
    return MAKE_ERROR_F(StatusCode::kInvalidArg, "failed to query CUDA pointer device: {}", driverError(driverResult));
  }
  if (pointerDevice != deviceId) {
    return MAKE_ERROR_F(StatusCode::kInvalidArg,
                        "CUDA pointer belongs to device {}, expected {}",
                        pointerDevice,
                        deviceId);
  }

  CUdeviceptr allocationBase = 0;
  size_t allocationSize = 0;
  driverResult = cuMemGetAddressRange(&allocationBase, &allocationSize, address);
  if (driverResult != CUDA_SUCCESS) {
    return MAKE_ERROR_F(StatusCode::kInvalidArg,
                        "cuMemGetAddressRange failed for CUDA pointer: {}",
                        driverError(driverResult));
  }
  if (address < allocationBase) {
    return makeError(StatusCode::kInvalidArg, "CUDA allocation range does not contain the pointer");
  }

  auto offset = static_cast<size_t>(address - allocationBase);
  if (allocationSize == 0 || offset > allocationSize || viewSize > allocationSize - offset) {
    return MAKE_ERROR_F(StatusCode::kInvalidArg,
                        "CUDA view [{}, {}) exceeds allocation size {}",
                        offset,
                        offset + viewSize,
                        allocationSize);
  }

  return DeviceMemoryView{
      .allocationBase = reinterpret_cast<void *>(allocationBase),
      .allocationSize = allocationSize,
      .offset = offset,
      .deviceId = pointerDevice,
  };
#else
  (void)devicePtr;
  (void)viewSize;
  (void)expectedDeviceId;
  return makeError(StatusCode::kNotImplemented, "CUDA/GDR is disabled in this build");
#endif
}

Result<DeviceMemoryView> prepareGdrMemory(void *devicePtr, size_t viewSize, int expectedDeviceId) {
#ifdef HF3FS_ENABLE_GDR
  auto view = inspectDeviceMemory(devicePtr, viewSize, expectedDeviceId);
  RETURN_ON_ERROR(view);

  auto guard = ScopedDevice::create(view->deviceId);
  RETURN_ON_ERROR(guard);

  unsigned int enabled = 0;
  auto allocationBase = reinterpret_cast<CUdeviceptr>(view->allocationBase);
  auto driverResult = cuPointerGetAttribute(&enabled, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS, allocationBase);
  if (driverResult != CUDA_SUCCESS) {
    return MAKE_ERROR_F(StatusCode::kIOError,
                        "failed to query CU_POINTER_ATTRIBUTE_SYNC_MEMOPS: {}",
                        driverError(driverResult));
  }
  if (enabled != 0) {
    return *view;
  }

  enabled = 1;
  driverResult = cuPointerSetAttribute(&enabled, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS, allocationBase);
  if (driverResult != CUDA_SUCCESS) {
    return MAKE_ERROR_F(StatusCode::kIOError,
                        "failed to enable CU_POINTER_ATTRIBUTE_SYNC_MEMOPS: {}",
                        driverError(driverResult));
  }
  return *view;
#else
  (void)devicePtr;
  (void)viewSize;
  (void)expectedDeviceId;
  return makeError(StatusCode::kNotImplemented, "CUDA/GDR is disabled in this build");
#endif
}

}  // namespace hf3fs::cuda
