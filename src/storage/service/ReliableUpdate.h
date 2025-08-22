#pragma once

#include "common/net/Transport.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/LockManager.h"
#include "common/utils/RobinHood.h"
#include "common/utils/Shards.h"
#include "common/utils/Size.h"
#include "fbs/storage/Common.h"
#include "storage/service/TargetMap.h"

namespace hf3fs::storage {

struct Components;
class StorageOperator;

class ReliableUpdate {
 public:
  struct Config : ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(clean_up_expired_clients, false);
    CONFIG_HOT_UPDATED_ITEM(expired_clients_timeout, 1_h);
  };
  ReliableUpdate(const Config &config, Components &components)
      : config_(config),
        components_(components) {}

  CoTask<IOResult> update(ServiceRequestContext &requestCtx,
                          UpdateReq &req,
                          net::IBSocket *ibSocket,
                          TargetPtr &target);

  Result<Void> cleanUpExpiredClients(const robin_hood::unordered_set<std::string> &activeClients);

  void beforeStop() { stopped_ = true; }

 private:
  ConstructLog<"storage::ReliableUpdate"> constructLog_;
  const Config &config_;
  Components &components_;
  std::atomic<bool> stopped_ = false;
  folly::coro::Mutex mutex_;

  struct ReqResult {
    SERDE_STRUCT_FIELD(channelSeqnum, ChannelSeqNum{0});
    SERDE_STRUCT_FIELD(requestId, RequestId{0});
    SERDE_STRUCT_FIELD(updateResult, IOResult{});
    SERDE_STRUCT_FIELD(succUpdateVer, ChunkVer{});
    SERDE_STRUCT_FIELD(generationId, uint32_t{});
  };

  struct ClientStatus {
    std::unordered_map<std::pair<ChainId, ChannelId>, ReqResult> channelMap;
    UtcTime lastUsedTime;
  };
  using ClientMap = std::unordered_map<ClientId, std::shared_ptr<ClientStatus>>;
  Shards<ClientMap, 1024> shards_;
};

}  // namespace hf3fs::storage
