#include "storage/service/BufferPool.h"

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/Task.h>
#include <sys/uio.h>

#include "common/monitor/Recorder.h"
#include "common/net/ib/RDMABuf.h"
#include "common/utils/MagicEnum.hpp"
#include "fbs/storage/Common.h"

namespace hf3fs::storage {
namespace {

void alignBuffer(net::RDMABuf &rdmabuf) {
  auto address = reinterpret_cast<uint64_t>(rdmabuf.ptr());
  auto remain = address % kAIOAlignSize;
  if (remain == 0) {
    return;
  }
  auto crop = kAIOAlignSize - remain;
  rdmabuf.advance(std::min(crop, rdmabuf.size()));
}

}  // namespace

Result<Void> BufferPool::init(CPUExecutorGroup &executor) {
  buffers_.clear();
  buffers_.reserve(UIO_MAXIOV);

  auto smallBufferResult =
      initBuffers(executor, config_.rdmabuf_size(), config_.rdmabuf_count(), UIO_MAXIOV / 2, buffers_);
  RETURN_AND_LOG_ON_ERROR(smallBufferResult);
  *freeIndex_.lock() = std::move(*smallBufferResult);

  bigBufferRegisterIndexStart_ = buffers_.size();

  auto bigBufferResult =
      initBuffers(executor, config_.big_rdmabuf_size(), config_.big_rdmabuf_count(), UIO_MAXIOV / 2, buffers_);
  RETURN_AND_LOG_ON_ERROR(bigBufferResult);
  *bigFreeIndex_.lock() = std::move(*bigBufferResult);

  iovecs_.clear();
  iovecs_.reserve(buffers_.size());
  for (auto &buf : buffers_) {
    iovecs_.push_back({(void *)buf.ptr(), buf.size()});
  }
  return Void{};
}

Result<std::vector<BufferIndex>> BufferPool::initBuffers(CPUExecutorGroup &executor,
                                                         Size rdmabufSize,
                                                         uint32_t rdmabufCount,
                                                         uint32_t limit,
                                                         std::vector<net::RDMABuf> &outBuffers) {
  size_t totalSize = rdmabufSize * rdmabufCount;
  size_t bufferCount = std::min(limit, rdmabufCount);
  size_t smallBufferCount = (totalSize / bufferCount + rdmabufSize - 1) / rdmabufSize;
  size_t bufferSize = smallBufferCount * rdmabufSize;
  auto pool = net::RDMABufPool::create(bufferSize, bufferCount);

  std::vector<folly::coro::TaskWithExecutor<net::RDMABuf>> tasks;
  tasks.reserve(bufferCount);
  for (auto i = 0u; i < bufferCount; ++i) {
    tasks.push_back(pool->allocate().scheduleOn(&executor.pickNext()));
  }
  XLOGF(INFO, "allocate {} * {} RDMA buffers started", bufferCount, Size{bufferSize});
  auto buffers = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(tasks)));
  XLOGF(INFO, "allocate {} * {} RDMA buffers finished", bufferCount, Size{bufferSize});

  std::vector<BufferIndex> freeIndex;
  freeIndex.reserve(rdmabufCount);
  for (auto &buf : buffers) {
    if (UNLIKELY(!buf)) {
      auto msg = fmt::format("storage init buffer pool failed");
      XLOG(ERR, msg);
      return makeError(StorageCode::kStorageInitFailed, std::move(msg));
    }
    alignBuffer(buf);

    BufferIndex bufferIndex;
    bufferIndex.registerIndex = outBuffers.size();
    outBuffers.push_back(buf);

    auto split = buf;
    for (; split.size() >= rdmabufSize; split.advance(rdmabufSize)) {
      bufferIndex.buffer = split.first(rdmabufSize);
      freeIndex.push_back(bufferIndex);
    }
  }
  return Result<std::vector<BufferIndex>>(std::move(freeIndex));
}

BufferPool::Buffer::~Buffer() {
  for (auto &index : indices_) {
    pool_->deallocate(index);
  }
}

Result<net::RDMABuf> BufferPool::Buffer::tryAllocate(uint32_t size) {
  if (indices_.empty() || current_.size() < size) {
    if (UNLIKELY(size > pool_->rdmabufSize_)) {
      return makeError(StorageCode::kBufferSizeExceeded);
    }
    if (LIKELY(pool_->semaphore_.try_wait())) {
      auto index = pool_->allocate();
      indices_.push_back(index);
      current_ = index.buffer;
    } else {
      return makeError(RPCCode::kRDMANoBuf);
    }
  }
  auto ret = current_.takeFirst(size);
  assert(ret);
  alignBuffer(current_);
  return ret;
}

CoTryTask<net::RDMABuf> BufferPool::Buffer::allocate(uint32_t size) {
  if (indices_.empty() || current_.size() < size) {
    if (UNLIKELY(size > pool_->bigRdmabufSize_)) {
      co_return makeError(StorageCode::kBufferSizeExceeded);
    } else if (UNLIKELY(size > pool_->rdmabufSize_)) {
      co_await pool_->bigSemaphore_.co_wait();
      auto index = pool_->allocateBig();
      indices_.push_back(index);
      current_ = index.buffer;
    } else {
      co_await pool_->semaphore_.co_wait();
      auto index = pool_->allocate();
      indices_.push_back(index);
      current_ = index.buffer;
    }
  }
  auto ret = current_.takeFirst(size);
  assert(ret);
  alignBuffer(current_);
  co_return ret;
}

void BufferPool::clear(CPUExecutorGroup &executor) {
  std::vector<folly::coro::TaskWithExecutor<void>> tasks;
  tasks.reserve(buffers_.size());
  for (auto &buffer : buffers_) {
    tasks.push_back(folly::coro::co_invoke([&, buf = std::move(buffer)]() mutable -> CoTask<void> {
                      buf = {};
                      co_return;
                    }).scheduleOn(&executor.pickNext()));
  }
  XLOGF(INFO, "deallocate {} RDMA buffers started", buffers_.size());
  folly::coro::blockingWait(folly::coro::collectAllRange(std::move(tasks)));
  XLOGF(INFO, "deallocate {} RDMA buffers finished", buffers_.size());
}

BufferIndex BufferPool::allocate() {
  auto guard = freeIndex_.lock();
  assert(!guard->empty());
  auto ret = guard->back();
  guard->pop_back();
  return ret;
}

BufferIndex BufferPool::allocateBig() {
  auto guard = bigFreeIndex_.lock();
  assert(!guard->empty());
  auto ret = guard->back();
  guard->pop_back();
  return ret;
}

void BufferPool::deallocate(const BufferIndex &index) {
  if (UNLIKELY(index.registerIndex >= bigBufferRegisterIndexStart_)) {
    bigFreeIndex_.lock()->push_back(index);
    bigSemaphore_.signal();
  } else {
    freeIndex_.lock()->push_back(index);
    semaphore_.signal();
  }
}

}  // namespace hf3fs::storage
