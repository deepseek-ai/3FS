#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/utils/Result.h"

namespace hf3fs::lib {

constexpr size_t kCudaIpcHandleBytes = 64;
using CudaIpcHandle = std::array<uint8_t, kCudaIpcHandleBytes>;

struct CudaIpcExport {
  void *allocationBase = nullptr;
  size_t allocationSize = 0;
  size_t offset = 0;
  CudaIpcHandle ipcHandle{};
};

Result<CudaIpcExport> exportCudaIpcMemory(void *devicePtr, size_t viewSize, int deviceId);

class CudaIpcMapping {
 public:
  static Result<CudaIpcMapping> open(int deviceId, const CudaIpcHandle &ipcHandle, size_t allocationSize);

  CudaIpcMapping(const CudaIpcMapping &) = delete;
  CudaIpcMapping &operator=(const CudaIpcMapping &) = delete;
  CudaIpcMapping(CudaIpcMapping &&other) noexcept;
  CudaIpcMapping &operator=(CudaIpcMapping &&other) noexcept;
  ~CudaIpcMapping();

  void *allocationBase() const { return allocationBase_; }
  size_t allocationSize() const { return allocationSize_; }
  Result<void *> view(size_t offset, size_t size) const;

 private:
  CudaIpcMapping(int deviceId, void *allocationBase, size_t allocationSize)
      : deviceId_(deviceId),
        allocationBase_(allocationBase),
        allocationSize_(allocationSize) {}

  void reset() noexcept;

  int deviceId_ = -1;
  void *allocationBase_ = nullptr;
  size_t allocationSize_ = 0;
};

}  // namespace hf3fs::lib
