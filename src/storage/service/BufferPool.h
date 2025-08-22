#pragma once

#include <folly/Synchronized.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/fibers/Semaphore.h>
#include <limits>

#include "common/net/ib/RDMABuf.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/ConstructLog.h"
#include "common/utils/Size.h"

namespace hf3fs::storage {

struct BufferIndex {
  uint32_t registerIndex;
  net::RDMABuf buffer;
};

class BufferPool {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_ITEM(rdmabuf_size, 4_MB);
    CONFIG_ITEM(rdmabuf_count, 1024u);
    CONFIG_ITEM(big_rdmabuf_size, 64_MB);
    CONFIG_ITEM(big_rdmabuf_count, 64u);
  };
  BufferPool(const Config &config)
      : config_(config),
        rdmabufSize_(config_.rdmabuf_size()),
        semaphore_(config_.rdmabuf_count()),
        bigRdmabufSize_(config_.big_rdmabuf_size()),
        bigSemaphore_(config_.big_rdmabuf_count()) {}

  Result<Void> init(CPUExecutorGroup &executor);

  auto &iovecs() const { return iovecs_; }

  class Buffer {
   public:
    explicit Buffer(BufferPool &pool)
        : pool_(&pool) {}
    Buffer(const Buffer &) = delete;
    Buffer(Buffer &&other) = default;
    Buffer &operator=(Buffer &&other) = default;
    ~Buffer();

    Result<net::RDMABuf> tryAllocate(uint32_t size);

    CoTryTask<net::RDMABuf> allocate(uint32_t size);

    auto index() const { return indices_.back().registerIndex; }

   private:
    BufferPool *pool_{};
    std::vector<BufferIndex> indices_;
    net::RDMABuf current_;
  };
  auto get() { return Buffer{*this}; }

  void clear(CPUExecutorGroup &executor);

 protected:
  static Result<std::vector<BufferIndex>> initBuffers(CPUExecutorGroup &executor,
                                                      Size rdmabufSize,
                                                      uint32_t rdmabufCount,
                                                      uint32_t limit,
                                                      std::vector<net::RDMABuf> &outBuffers);

  BufferIndex allocate();

  BufferIndex allocateBig();

  void deallocate(const BufferIndex &index);

 private:
  ConstructLog<"storage::BufferPool"> constructLog_;
  const Config &config_;
  Size rdmabufSize_;
  std::vector<net::RDMABuf> buffers_;
  std::vector<struct iovec> iovecs_;
  folly::fibers::Semaphore semaphore_;
  folly::Synchronized<std::vector<BufferIndex>, std::mutex> freeIndex_;

  Size bigRdmabufSize_;
  uint32_t bigBufferRegisterIndexStart_ = 0;
  folly::fibers::Semaphore bigSemaphore_;
  folly::Synchronized<std::vector<BufferIndex>, std::mutex> bigFreeIndex_;
};

}  // namespace hf3fs::storage
