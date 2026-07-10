#include "RDMABufAccelerator.h"

#include <folly/logging/xlog.h>
#include <utility>

#ifdef HF3FS_GDR_ENABLED
#include <cuda_runtime.h>
#endif

#include "common/monitor/Recorder.h"

namespace hf3fs::net {

namespace {
monitor::CountRecorder gpuRdmaBufMem("common.ib.gpu_rdma_buf_mem", {}, false);
}  // namespace

// RDMABufAccelerator implementation

RDMABufAccelerator::RDMABufAccelerator(RDMABufAccelerator &&other) noexcept
    : region_(std::move(other.region_)),
      begin_(std::exchange(other.begin_, nullptr)),
      length_(std::exchange(other.length_, 0)) {}

RDMABufAccelerator &RDMABufAccelerator::operator=(RDMABufAccelerator &&other) noexcept {
  if (this != &other) {
    release();
    region_ = std::move(other.region_);
    begin_ = std::exchange(other.begin_, nullptr);
    length_ = std::exchange(other.length_, 0);
  }
  return *this;
}

RDMABufAccelerator::~RDMABufAccelerator() { release(); }

void RDMABufAccelerator::release() {
  region_.reset();
  begin_ = nullptr;
  length_ = 0;
}

RDMABufAccelerator RDMABufAccelerator::createFromGpuPointer(void *devicePtr, size_t size, int deviceId) {
  if (!devicePtr || size == 0 || deviceId < 0) {
    XLOGF(ERR, "Invalid GPU pointer parameters: ptr={}, size={}, device={}", devicePtr, size, deviceId);
    return RDMABufAccelerator();
  }

  AcceleratorMemoryDescriptor desc;
  desc.devicePtr = devicePtr;
  desc.size = size;
  desc.deviceId = deviceId;

  return createFromDescriptor(desc);
}

RDMABufAccelerator RDMABufAccelerator::createFromDescriptor(const AcceleratorMemoryDescriptor &desc) {
  if (!desc.isValid()) {
    XLOGF(ERR, "Invalid GPU memory descriptor");
    return RDMABufAccelerator();
  }

  if (!GDRManager::instance().isAvailable()) {
    XLOGF(ERR, "GDR not available");
    return RDMABufAccelerator();
  }

  // Try to get from cache or create new region
  auto *cache = GDRManager::instance().getRegionCache();
  if (!cache) {
    XLOGF(ERR, "GDR region cache not available");
    return RDMABufAccelerator();
  }
  auto result = cache->getOrCreate(desc);
  if (!result) {
    XLOGF(ERR, "Failed to create GPU memory region: {}", result.error().message());
    return RDMABufAccelerator();
  }

  auto region = *result;
  gpuRdmaBufMem.addSample(desc.size);

  return RDMABufAccelerator(region, static_cast<uint8_t *>(desc.devicePtr), desc.size);
}

RDMARemoteBuf RDMABufAccelerator::toRemoteBuf() const {
  if (!valid()) {
    return RDMARemoteBuf();
  }

  std::array<RDMARemoteBuf::Rkey, IBDevice::kMaxDeviceCnt> rkeys;
  for (auto &rkey : rkeys) {
    rkey.devId = -1;
    rkey.rkey = 0;
  }

  size_t devs = 0;
  for (const auto &dev : IBDevice::all()) {
    if (dev->id() >= IBDevice::kMaxDeviceCnt) continue;

    auto mr = region_->getMR(dev->id());
    if (mr) {
      rkeys[devs].rkey = mr->rkey;
      rkeys[devs].devId = dev->id();
      ++devs;
    }
  }

  if (devs == 0) {
    XLOGF(WARN, "No rkeys available for GPU buffer");
    return RDMARemoteBuf();
  }

  return RDMARemoteBuf(reinterpret_cast<uint64_t>(begin_), length_, rkeys);
}

RDMABufAccelerator RDMABufAccelerator::subrange(size_t offset, size_t length) const {
  if (!valid()) {
    return RDMABufAccelerator();
  }

  if (offset > length_ || length > length_ - offset) {
    XLOGF(WARN, "Subrange exceeds buffer bounds: offset={}, length={}, size={}", offset, length, length_);
    return RDMABufAccelerator();
  }

  RDMABufAccelerator view(region_, begin_ + offset, length);
  return view;
}

void RDMABufAccelerator::sync(int direction) const {
  if (!valid()) {
    return;
  }

#ifdef HF3FS_GDR_ENABLED
  cudaError_t err = cudaSetDevice(region_->deviceId());
  if (err != cudaSuccess) {
    XLOGF(WARN, "cudaSetDevice({}) failed: {}", region_->deviceId(), cudaGetErrorString(err));
    return;
  }
  err = cudaDeviceSynchronize();
  if (err != cudaSuccess) {
    XLOGF(WARN, "cudaDeviceSynchronize failed: {}", cudaGetErrorString(err));
  }
#else
  (void)direction;
#endif

  XLOGF(DBG, "GPU buffer sync: direction={}, ptr={}, size={}", direction, static_cast<void *>(begin_), length_);
}

}  // namespace hf3fs::net
