#pragma once

#include <atomic>
#include <condition_variable>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <mutex>

#include "client/storage/UpdateChannelAllocator.h"
#include "common/serde/Serde.h"
#include "common/utils/ConcurrencyLimiter.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/CoroutinesPool.h"
#include "common/utils/Duration.h"
#include "common/utils/Shards.h"
#include "fbs/storage/Common.h"
#include "storage/service/TargetMap.h"

namespace hf3fs::storage {

struct Components;

class ResyncWorker {
 public:
  enum FullSyncLevel {
    NONE,
    HEAVY,  // sync all.
  };
  struct Config : ConfigBase<Config> {
    CONFIG_ITEM(num_threads, 16ul);
    CONFIG_ITEM(num_channels, 1024u);
    CONFIG_HOT_UPDATED_ITEM(batch_size, 16u);
    CONFIG_HOT_UPDATED_ITEM(sync_start_timeout, 10_s);
    CONFIG_HOT_UPDATED_ITEM(full_sync_chains, std::set<uint32_t>{});  // full sync all chains if it is empty.
    CONFIG_HOT_UPDATED_ITEM(full_sync_level, FullSyncLevel::NONE);
    CONFIG_OBJ(pool, CoroutinesPoolBase::Config);
    CONFIG_OBJ(batch_concurrency_limiter, ConcurrencyLimiterConfig, [](auto &c) { c.set_max_concurrency(64); });
  };
  ResyncWorker(const Config &config, Components &components);

  // start resync worker.
  Result<Void> start();

  // stop resync worker. End all sync tasks immediately.
  Result<Void> stopAndJoin();

 protected:
  void loop();

  // handle sync job.
  CoTryTask<void> handleSync(VersionedChainId vChainId);

  // forward sync request.
  CoTryTask<void> forward(const TargetPtr &target,
                          const monitor::TagSet &tag,
                          const ClientId &clientId,
                          ChunkId chunkId,
                          UpdateType updateType,
                          uint32_t chunkSize);

 private:
  ConstructLog<"storage::ResyncWorker"> constructLog_;
  const Config &config_;
  Components &components_;
  folly::CPUThreadPoolExecutor executors_;
  CoroutinesPool<VersionedChainId> pool_;
  client::UpdateChannelAllocator updateChannelAllocator_;
  ConcurrencyLimiter<uint32_t> batchConcurrencyLimiter_;

  std::mutex mutex_;
  std::condition_variable cond_;
  std::atomic<bool> stopping_ = false;
  std::atomic<bool> started_ = false;
  std::atomic<bool> stopped_ = false;

  struct SyncingStatus {
    SERDE_STRUCT_FIELD(isSyncing, false);
    SERDE_STRUCT_FIELD(lastSyncingTime, RelativeTime{});
  };
  using SyncingChainIds = robin_hood::unordered_map<ChainId, SyncingStatus>;
  Shards<SyncingChainIds, 32> shards_;
  std::atomic_uint64_t requestId_;
};

}  // namespace hf3fs::storage
