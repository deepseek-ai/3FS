#pragma once

#include "client/storage/StorageMessenger.h"
#include "common/net/Client.h"
#include "common/net/ib/RDMABuf.h"
#include "common/utils/ConfigBase.h"
#include "fbs/storage/Common.h"
#include "storage/update/UpdateJob.h"

namespace hf3fs::storage {

struct Components;
struct Target;

class ReliableForwarding {
 public:
  struct Config : ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(retry_first_wait, 100_ms);
    CONFIG_HOT_UPDATED_ITEM(retry_max_wait, 1000_ms);
    CONFIG_HOT_UPDATED_ITEM(retry_total_time, 60_s);
    CONFIG_HOT_UPDATED_ITEM(max_inline_forward_bytes, Size{});
  };

  ReliableForwarding(const Config &config, Components &components)
      : config_(config),
        components_(components) {}

  Result<Void> init();

  void beforeStop() { stopped_ = true; }

  Result<Void> stopAndJoin();

  CoTask<IOResult> forwardWithRetry(ServiceRequestContext &requestCtx,
                                    const UpdateReq &req,
                                    const net::RDMARemoteBuf &rdmabuf,
                                    const ChunkEngineUpdateJob &chunkEngineJob,
                                    TargetPtr &target,
                                    CommitIO &commitIO,
                                    bool allowOutdatedChainVer = true);

  CoTask<IOResult> forward(const UpdateReq &req,
                           uint32_t retryCount,
                           const net::RDMARemoteBuf &rdmabuf,
                           const ChunkEngineUpdateJob &chunkEngineJob,
                           TargetPtr &target,
                           CommitIO &commitIO,
                           std::chrono::milliseconds timeout);

  CoTask<IOResult> doForward(const UpdateReq &req,
                             const net::RDMARemoteBuf &rdmabuf,
                             const ChunkEngineUpdateJob &chunkEngineJob,
                             uint32_t retryCount,
                             const Target &target,
                             bool &isSyncing,
                             std::chrono::milliseconds timeout);

 private:
  ConstructLog<"storage::ReliableForwarding"> constructLog_;
  const Config &config_;
  Components &components_;
  std::atomic<bool> stopped_ = false;
};

}  // namespace hf3fs::storage
