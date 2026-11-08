#pragma once

#include <cstddef>

#include "common/utils/Result.h"

namespace hf3fs::cuda {

struct DeviceMemoryView {
  void *allocationBase = nullptr;
  size_t allocationSize = 0;
  size_t offset = 0;
  int deviceId = -1;
};

class ScopedDevice {
 public:
  ScopedDevice(const ScopedDevice &) = delete;
  ScopedDevice &operator=(const ScopedDevice &) = delete;
  ScopedDevice(ScopedDevice &&other) noexcept;
  ScopedDevice &operator=(ScopedDevice &&other) noexcept;
  ~ScopedDevice();

  static Result<ScopedDevice> create(int deviceId);

 private:
  explicit ScopedDevice(int previousDevice, bool restore)
      : previousDevice_(previousDevice),
        restore_(restore) {}

  void restore() noexcept;

  int previousDevice_ = -1;
  bool restore_ = false;
};

Result<int> deviceCount();
/** Local CUDA IPC prerequisites only; this does not probe an HCA or ibv_reg_mr. */
Result<bool> supportsIpc(int deviceId);

/** Validate a CUDA device-memory view; pass -1 to discover its device. */
Result<DeviceMemoryView> inspectDeviceMemory(void *devicePtr, size_t viewSize, int expectedDeviceId);

/**
 * Validate a CUDA device-memory view and enable the allocation-wide synchronization
 * behavior required before publishing or directly registering it for GPUDirect RDMA.
 */
Result<DeviceMemoryView> prepareGdrMemory(void *devicePtr, size_t viewSize, int expectedDeviceId);

}  // namespace hf3fs::cuda
