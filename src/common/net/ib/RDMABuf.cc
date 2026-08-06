#include "RDMABuf.h"

#include <cstdint>
#include <folly/experimental/coro/Mutex.h>
#include <folly/experimental/coro/Timeout.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <mutex>
#include <unistd.h>
#include <utility>

#include "common/cuda/CudaMemory.h"
#include "common/monitor/Recorder.h"
#include "common/net/ib/IBDevice.h"
#include "memory/common/GlobalMemoryAllocator.h"

namespace hf3fs::net {

namespace {
monitor::CountRecorder rdmaBufMem("common.ib.rdma_buf_mem", {}, false);
}

/** RDMABuf */
void RDMABuf::Inner::deallocate(Inner *ptr) {
  if (ptr) {
    auto pool = ptr->pool_.lock();
    if (pool) {
      pool->deallocate(ptr);
      return;
    }
  }
  delete ptr;
}

RDMABuf RDMABuf::allocate(size_t size, std::weak_ptr<RDMABufPool> pool) {
  RDMABuf::Inner inner(std::move(pool), size);
  if (inner.init() != 0) {
    return RDMABuf();
  }
  return RDMABuf(std::shared_ptr<RDMABuf::Inner>(new RDMABuf::Inner(std::move(inner)), RDMABuf::Inner::deallocate));
}

RDMABuf RDMABuf::createFromUserBuffer(uint8_t *buf, size_t len) {
  RDMABuf::Inner inner(buf, len);
  if (inner.registerMemory() != 0) {
    return RDMABuf();
  }
  return RDMABuf(std::shared_ptr<RDMABuf::Inner>(new RDMABuf::Inner(std::move(inner)), RDMABuf::Inner::deallocate));
}

Result<RDMABuf> RDMABuf::createFromCudaBuffer(uint8_t *ptr,
                                              size_t len,
                                              int expectedCudaDeviceId,
                                              BackingOwner backingOwner) {
#ifdef HF3FS_ENABLE_GDR
  // NVIDIA requires SYNC_MEMOPS to be enabled before a CUDA allocation is
  // pinned for GPUDirect RDMA. Do this in the registering process as well as
  // in the exporter, because an IPC mapping has its own CUDA context.
  auto view = cuda::prepareGdrMemory(ptr, len, expectedCudaDeviceId);
  RETURN_ON_ERROR(view);

  if (!IBManager::initialized()) {
    return makeError(RPCCode::kIBDeviceNotInitialized, "IB is not initialized");
  }
  if (IBDevice::all().empty()) {
    return makeError(RPCCode::kIBDeviceNotFound, "no active IB device is available");
  }

  // Keep the imported/allocation device current while nvidia_peermem pins the
  // view through ibv_reg_mr; the caller's previous device is restored after
  // registration completes.
  auto deviceGuard = cuda::ScopedDevice::create(view->deviceId);
  RETURN_ON_ERROR(deviceGuard);

  RDMABuf::Inner inner(ptr, len, view->deviceId, std::move(backingOwner));
  if (inner.registerMemory() != 0) {
    return makeError(StatusCode::kIOError, "failed to register CUDA memory with every active IB device");
  }
  return RDMABuf(std::shared_ptr<RDMABuf::Inner>(new RDMABuf::Inner(std::move(inner)), RDMABuf::Inner::deallocate));
#else
  (void)ptr;
  (void)len;
  (void)expectedCudaDeviceId;
  (void)backingOwner;
  return makeError(StatusCode::kNotImplemented, "CUDA/GDR is disabled in this build");
#endif
}

RDMABuf::Inner::~Inner() {
  XLOGF(DBG, "RDMABuf free and deregister, ptr {}", (void *)ptr_);
  for (auto &dev : IBDevice::all()) {
    XLOGF_IF(FATAL, UNLIKELY(dev->id() >= IBDevice::kMaxDeviceCnt), "{} > {}", dev->id(), IBDevice::kMaxDeviceCnt);
    auto mr = mrs_.at(dev->id());
    if (mr) {
      dev->deregMemory(mr);
    }
  }
  if (ptr_ && ownsAllocation_) {
    rdmaBufMem.addSample(-capacity_);
    hf3fs::memory::deallocate(ptr_);
  }
}

RDMABufMR RDMABuf::Inner::getMR(int dev) const {
  XLOGF_IF(FATAL, UNLIKELY((size_t)dev >= IBDevice::kMaxDeviceCnt), "{} >= {}", dev, IBDevice::kMaxDeviceCnt);
  return mrs_.at(dev);
}

bool RDMABuf::Inner::getRkeys(std::array<RDMARemoteBuf::Rkey, IBDevice::kMaxDeviceCnt> &rkeys) const {
  size_t devs = 0;
  for (auto &dev : IBDevice::all()) {
    XLOGF_IF(FATAL,
             UNLIKELY(dev->id() >= IBDevice::kMaxDeviceCnt || (devs++) >= IBDevice::kMaxDeviceCnt),
             "{} >= {} || {} >= {}, {}",
             dev->id(),
             IBDevice::kMaxDeviceCnt,
             devs,
             IBDevice::kMaxDeviceCnt,
             IBDevice::all().size());
    auto mr = mrs_.at(dev->id());
    if (!mr) {
      return false;
    }
    rkeys[dev->id()] = RDMARemoteBuf::Rkey{
        .rkey = mr->rkey,
        .devId = dev->id(),
    };
  }
  return true;
}

int RDMABuf::Inner::allocateMemory() {
  static size_t kPageSize = sysconf(_SC_PAGESIZE);

  ptr_ = (uint8_t *)hf3fs::memory::memalign(kPageSize, capacity_);
  if (!ptr_) {
    XLOGF(CRITICAL, "RDMABuf failed to allocate memory, len {}.", capacity_);
    return -ENOMEM;
  }
  rdmaBufMem.addSample(capacity_);

  XLOGF(DBG, "RDMABuf allocated, ptr {}, capacity {}", (void *)ptr_, capacity_);
  return 0;
}

int RDMABuf::Inner::registerMemory() {
  size_t devs = 0;
  for (auto &dev : IBDevice::all()) {
    XLOGF_IF(FATAL,
             UNLIKELY(dev->id() >= IBDevice::kMaxDeviceCnt || (devs++) >= IBDevice::kMaxDeviceCnt),
             "{} >= {} || {} >= {}, {}",
             dev->id(),
             IBDevice::kMaxDeviceCnt,
             devs,
             IBDevice::kMaxDeviceCnt,
             IBDevice::all().size());
    auto mr = dev->regMemory(ptr_, capacity_, kAccessFlags);
    if (!mr) {
      return -1;
    }
    mrs_[dev->id()] = mr;
  }

  XLOGF(DBG, "RDMABuf registered, ptr {}, capacity {}", (void *)ptr_, capacity_);
  return 0;
}

int RDMABuf::Inner::init() {
  int ret = 0;
  if ((ret = allocateMemory())) return ret;
  if ((ret = registerMemory())) return ret;
  return 0;
}

/** RDMABufPool */
RDMABufPool::~RDMABufPool() {
  std::lock_guard<std::mutex> lock(mutex_);
  while (!freeList_.empty()) {
    auto *buf = freeList_.front();
    freeList_.pop_front();
    delete buf;
  }
}

CoTask<RDMABuf> RDMABufPool::allocate(std::optional<folly::Duration> timeout) {
  // wait available RDMA buf
  if (UNLIKELY(!sem_.try_wait())) {
    if (timeout.has_value()) {
      auto result = co_await folly::coro::co_awaitTry(folly::coro::timeout(sem_.co_wait(), timeout.value()));
      if (result.hasException()) {
        co_return RDMABuf();
      }
    } else {
      co_await sem_.co_wait();
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!freeList_.empty()) {
      auto *buf = freeList_.back();
      freeList_.pop_back();
      co_return RDMABuf(buf);
    }
  }

  co_return RDMABuf::allocate(bufSize(), shared_from_this());
}

void RDMABufPool::deallocate(RDMABuf::Inner *buf) {
  std::lock_guard<std::mutex> lock(mutex_);
  freeList_.push_back(buf);
  sem_.signal();
}

}  // namespace hf3fs::net
