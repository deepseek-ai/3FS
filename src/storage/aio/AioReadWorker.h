#pragma once

#include <atomic>
#include <folly/Random.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <memory>
#include <vector>

#include "common/utils/BoundedQueue.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Result.h"
#include "storage/aio/AioStatus.h"
#include "storage/aio/BatchReadJob.h"
#include "storage/store/StorageTargets.h"

namespace hf3fs::storage {

class AioReadWorker {
 public:
  enum class IoEngine {
    libaio,
    io_uring,
    random,
  };

  class Config : public ConfigBase<Config> {
    CONFIG_ITEM(num_threads, 32ul);
    CONFIG_ITEM(queue_size, 4096u);
    CONFIG_ITEM(max_events, 512u);
    CONFIG_ITEM(enable_io_uring, true);
    CONFIG_HOT_UPDATED_ITEM(min_complete, 128u);
    CONFIG_HOT_UPDATED_ITEM(wait_all_inflight, false);      // deprecated.
    CONFIG_HOT_UPDATED_ITEM(inflight_control_offset, 128);  // deprecated.
    CONFIG_HOT_UPDATED_ITEM(ioengine, IoEngine::libaio);

   public:
    inline bool useIoUring() const {
      if (!enable_io_uring()) {
        return false;
      }
      switch (ioengine()) {
        case IoEngine::io_uring:
          return true;
        case IoEngine::libaio:
          return false;
        case IoEngine::random:
          return folly::Random::rand32() & 1;
      }
    }
  };

  AioReadWorker(const Config &config)
      : config_(config),
        queue_(config.queue_size()),
        executors_(std::make_pair(config_.num_threads(), config_.num_threads()),
                   std::make_shared<folly::NamedThreadFactory>("AioRead")) {}
  ~AioReadWorker();

  CoTask<void> enqueue(AioReadJobIterator job) { co_await queue_.co_enqueue(job); }

  Result<Void> start(const std::vector<int> &fds, const std::vector<struct iovec> &iovecs);

  Result<Void> stopAndJoin();

 protected:
  Result<Void> run(AioStatus &aioStatus, IoUringStatus &ioUringStatus);

 private:
  ConstructLog<"storage::AioReadWorker"> constructLog_;
  const Config &config_;
  BoundedQueue<AioReadJobIterator> queue_;

  folly::CPUThreadPoolExecutor executors_;
  std::atomic<uint32_t> initialized_{};
  folly::Synchronized<Result<Void>, std::mutex> initResult_{Void{}};
};

}  // namespace hf3fs::storage
