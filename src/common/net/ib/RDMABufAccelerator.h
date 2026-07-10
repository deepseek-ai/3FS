#pragma once

#include <cstdint>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>
#include <variant>

#include "common/net/ib/AcceleratorMemory.h"
#include "common/net/ib/RDMABuf.h"

namespace hf3fs::net {

/**
 * RDMA buffer wrapper for GPU memory
 *
 * RDMABufAccelerator extends the RDMABuf concept to support GPU device memory.
 * It handles GPU memory registration with IB devices for direct RDMA
 * transfers (GPU Direct RDMA).
 *
 * Key differences from RDMABuf:
 * - Memory is allocated on GPU device (not host)
 * - Uses nvidia_peermem for memory registration
 * - May require synchronization between GPU and RDMA operations
 */
class RDMABufAccelerator {
 public:
  RDMABufAccelerator() = default;

  // Non-copyable but movable.
  RDMABufAccelerator(const RDMABufAccelerator &) = delete;
  RDMABufAccelerator &operator=(const RDMABufAccelerator &) = delete;
  RDMABufAccelerator(RDMABufAccelerator &&other) noexcept;
  RDMABufAccelerator &operator=(RDMABufAccelerator &&other) noexcept;
  ~RDMABufAccelerator();

  struct OwnerSnapshot {
    std::shared_ptr<AcceleratorMemoryRegion> region;
  };

  /**
   * Create from existing GPU device pointer
   *
   * The caller retains ownership of the GPU memory.
   * The GPU memory must remain valid for the lifetime of this object.
   *
   * @param devicePtr GPU device pointer
   * @param size Size of the memory region
   * @param deviceId CUDA device ID
   * @return The created buffer, or invalid buffer on failure
   */
  static RDMABufAccelerator createFromGpuPointer(void *devicePtr, size_t size, int deviceId);

  /**
   * Create from GPU memory descriptor
   *
   * @param desc GPU memory descriptor with all necessary information
   * @return The created buffer, or invalid buffer on failure
   */
  static RDMABufAccelerator createFromDescriptor(const AcceleratorMemoryDescriptor &desc);

  /**
   * Check if the buffer is valid and usable
   */
  bool valid() const { return region_ != nullptr; }
  explicit operator bool() const { return valid(); }

  /**
   * Get the base GPU device pointer for the underlying allocation.
   * After advance()/subrange(), this still returns the original base.
   * Use ptr() for the current position.
   */
  void *devicePtr() const { return region_ ? region_->devicePtr() : nullptr; }

  /**
   * Get the current data pointer (respects advance/subrange offsets).
   * Returns a GPU device pointer; NOT CPU-dereferenceable.
   */
  uint8_t *ptr() { return begin_; }
  const uint8_t *ptr() const { return begin_; }

  /**
   * Get the total capacity of the buffer
   */
  size_t capacity() const { return region_ ? region_->size() : 0; }

  /**
   * Get the current size of the buffer (may be less than capacity)
   */
  size_t size() const { return length_; }

  /**
   * Check if the buffer is empty
   */
  bool empty() const { return size() == 0; }

  /**
   * Get the GPU device ID
   */
  int deviceId() const { return region_ ? region_->deviceId() : -1; }

  /**
   * Get the memory region for a specific IB device
   *
   * @param devId IB device ID
   * @return Memory region pointer or nullptr
   */
  ibv_mr *getMR(int devId) const { return region_ ? region_->getMR(devId) : nullptr; }

  /**
   * Get the rkey for a specific IB device
   */
  std::optional<uint32_t> getRkey(int devId) const { return region_ ? region_->getRkey(devId) : std::nullopt; }

  /**
   * Convert to RDMARemoteBuf for remote RDMA operations
   *
   * The returned RDMARemoteBuf contains the device address and rkeys
   * needed for remote RDMA read/write operations.
   */
  RDMARemoteBuf toRemoteBuf() const;

  /**
   * Reset the buffer range to full capacity
   */
  void resetRange() {
    if (region_) {
      begin_ = static_cast<uint8_t *>(region_->devicePtr());
      length_ = region_->size();
    }
  }

  /**
   * Advance the start pointer by n bytes
   * @return false if n > size()
   */
  bool advance(size_t n) {
    if (n > length_) return false;
    begin_ += n;
    length_ -= n;
    return true;
  }

  /**
   * Reduce the size by n bytes from the end
   * @return false if n > size()
   */
  bool subtract(size_t n) {
    if (n > length_) return false;
    length_ -= n;
    return true;
  }

  /**
   * Create a subrange view of this buffer
   */
  RDMABufAccelerator subrange(size_t offset, size_t length) const;

  /**
   * Get the first `length` bytes
   */
  RDMABufAccelerator first(size_t length) const { return subrange(0, length); }

  /**
   * Take the first `length` bytes (modifies this buffer)
   */
  RDMABufAccelerator takeFirst(size_t length) {
    auto buf = first(length);
    advance(length);
    return buf;
  }

  /**
   * Get the last `length` bytes
   */
  RDMABufAccelerator last(size_t length) const {
    if (length > length_) return RDMABufAccelerator();
    return subrange(length_ - length, length);
  }

  /**
   * Take the last `length` bytes (modifies this buffer)
   */
  RDMABufAccelerator takeLast(size_t length) {
    auto buf = last(length);
    subtract(length);
    return buf;
  }

  /**
   * Check if a pointer range is contained within this buffer
   */
  bool contains(const uint8_t *data, uint32_t len) const {
    auto *basePtr = ptr();
    if (!basePtr || !data) {
      return false;
    }
    auto base = reinterpret_cast<uintptr_t>(basePtr);
    auto dataAddr = reinterpret_cast<uintptr_t>(data);
    auto cap = capacity();
    return dataAddr >= base && dataAddr - base <= cap && len <= cap - (dataAddr - base);
  }

  /**
   * Synchronize GPU memory for RDMA operations
   *
   * @param direction 0 = before RDMA (ensure GPU writes visible to RDMA)
   *                  1 = after RDMA (ensure RDMA writes visible to GPU)
   */
  void sync(int direction) const;

  OwnerSnapshot ownerSnapshot() const { return OwnerSnapshot{region_}; }

 private:
  RDMABufAccelerator(std::shared_ptr<AcceleratorMemoryRegion> region, uint8_t *begin, size_t length)
      : region_(std::move(region)),
        begin_(begin),
        length_(length) {}

  std::shared_ptr<AcceleratorMemoryRegion> region_;
  uint8_t *begin_ = nullptr;
  size_t length_ = 0;

  void release();
};

/**
 * Unified RDMA buffer that can hold either host or GPU memory
 *
 * This is a variant type that can represent either a regular RDMABuf
 * (host memory) or an RDMABufAccelerator (GPU memory), providing a uniform
 * interface for code that needs to handle both.
 *
 */
class RDMABufUnified {
 public:
  enum class Type {
    Empty,
    Host,
    Gpu,
  };

  RDMABufUnified() = default;

  explicit RDMABufUnified(RDMABuf hostBuf)
      : buffer_(std::in_place_type<RDMABuf>, std::move(hostBuf)) {}

  explicit RDMABufUnified(RDMABufAccelerator gpuBuf)
      : buffer_(std::in_place_type<RDMABufAccelerator>, std::move(gpuBuf)) {}

  RDMABufUnified(const RDMABufUnified &) = delete;
  RDMABufUnified &operator=(const RDMABufUnified &) = delete;
  RDMABufUnified(RDMABufUnified &&) noexcept = default;
  RDMABufUnified &operator=(RDMABufUnified &&) noexcept = default;

  Type type() const {
    XLOGF_IF(FATAL, buffer_.valueless_by_exception(), "RDMABufUnified is valueless");
    if (std::holds_alternative<RDMABuf>(buffer_)) {
      return Type::Host;
    }
    if (std::holds_alternative<RDMABufAccelerator>(buffer_)) {
      return Type::Gpu;
    }
    return Type::Empty;
  }
  bool isHost() const { return std::holds_alternative<RDMABuf>(buffer_); }
  bool isGpu() const { return std::holds_alternative<RDMABufAccelerator>(buffer_); }
  /** Alias for isGpu() — matches design doc naming convention. */
  bool isDevice() const { return isGpu(); }
  bool valid() const {
    switch (type()) {
      case Type::Host:
        return asHost().valid();
      case Type::Gpu:
        return asGpu().valid();
      default:
        return false;
    }
  }

  explicit operator bool() const { return valid(); }

  // Access the underlying buffer (caller must check type first)
  RDMABuf &asHost() {
    XLOGF_IF(FATAL, !isHost(), "RDMABufUnified type {} is not Host", static_cast<int>(type()));
    return std::get<RDMABuf>(buffer_);
  }
  const RDMABuf &asHost() const {
    XLOGF_IF(FATAL, !isHost(), "RDMABufUnified type {} is not Host", static_cast<int>(type()));
    return std::get<RDMABuf>(buffer_);
  }
  RDMABufAccelerator &asGpu() {
    XLOGF_IF(FATAL, !isGpu(), "RDMABufUnified type {} is not Gpu", static_cast<int>(type()));
    return std::get<RDMABufAccelerator>(buffer_);
  }
  const RDMABufAccelerator &asGpu() const {
    XLOGF_IF(FATAL, !isGpu(), "RDMABufUnified type {} is not Gpu", static_cast<int>(type()));
    return std::get<RDMABufAccelerator>(buffer_);
  }

  uint8_t *ptr() {
    switch (type()) {
      case Type::Host:
        return asHost().ptr();
      case Type::Gpu:
        return asGpu().ptr();
      default:
        return nullptr;
    }
  }
  const uint8_t *ptr() const {
    switch (type()) {
      case Type::Host:
        return asHost().ptr();
      case Type::Gpu:
        return asGpu().ptr();
      default:
        return nullptr;
    }
  }

  size_t size() const {
    switch (type()) {
      case Type::Host:
        return asHost().size();
      case Type::Gpu:
        return asGpu().size();
      default:
        return 0;
    }
  }

  size_t capacity() const {
    switch (type()) {
      case Type::Host:
        return asHost().capacity();
      case Type::Gpu:
        return asGpu().capacity();
      default:
        return 0;
    }
  }

  ibv_mr *getMR(int devId) const {
    switch (type()) {
      case Type::Host:
        return asHost().getMR(devId);
      case Type::Gpu:
        return asGpu().getMR(devId);
      default:
        return nullptr;
    }
  }

  RDMARemoteBuf toRemoteBuf() const {
    switch (type()) {
      case Type::Host:
        return asHost().toRemoteBuf();
      case Type::Gpu:
        return asGpu().toRemoteBuf();
      default:
        return RDMARemoteBuf();
    }
  }

 private:
  std::variant<std::monostate, RDMABuf, RDMABufAccelerator> buffer_;
};

}  // namespace hf3fs::net
