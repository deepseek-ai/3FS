#pragma once

#include <folly/concurrency/AtomicSharedPtr.h>

#include "client/mgmtd/MgmtdClientForServer.h"
#include "client/storage/StorageMessenger.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/DynamicCoroutinesPool.h"
#include "common/utils/LockManager.h"
#include "common/utils/RobinHood.h"
#include "fbs/storage/Service.h"
#include "storage/aio/AioReadWorker.h"
#include "storage/service/BufferPool.h"
#include "storage/service/StorageOperator.h"
#include "storage/service/TargetMap.h"
#include "storage/store/StorageTargets.h"
#include "storage/sync/ResyncWorker.h"
#include "storage/worker/AllocateWorker.h"
#include "storage/worker/CheckWorker.h"
#include "storage/worker/DumpWorker.h"
#include "storage/worker/PunchHoleWorker.h"
#include "storage/worker/SyncMetaKvWorker.h"

namespace hf3fs::storage {

class ReliableForwarding;

struct Components {
  struct Config : public ConfigBase<Config> {
    CONFIG_OBJ(base, net::Server::Config, [](net::Server::Config &c) {
      c.set_groups_length(2);
      c.groups(0).listener().set_listen_port(8000);
      c.groups(0).set_network_type(net::Address::RDMA);
      c.groups(0).set_services({"StorageSerde"});

      c.groups(1).set_network_type(net::Address::TCP);
      c.groups(1).listener().set_listen_port(9000);
      c.groups(1).set_use_independent_thread_pool(true);
      c.groups(1).set_services({"Core"});

      c.thread_pool().set_num_io_threads(32);
      c.thread_pool().set_num_proc_threads(32);
    });

    CONFIG_OBJ(client, net::Client::Config);
    CONFIG_OBJ(mgmtd, hf3fs::client::MgmtdClientForServer::Config);
    CONFIG_OBJ(targets, StorageTargets::Config);
    CONFIG_OBJ(storage, StorageOperator::Config);
    CONFIG_OBJ(reliable_forwarding, ReliableForwarding::Config);
    CONFIG_OBJ(reliable_update, ReliableUpdate::Config);
    CONFIG_OBJ(buffer_pool, BufferPool::Config);
    CONFIG_OBJ(aio_read_worker, AioReadWorker::Config);
    CONFIG_OBJ(sync_worker, ResyncWorker::Config);
    CONFIG_OBJ(check_worker, CheckWorker::Config);
    CONFIG_OBJ(dump_worker, DumpWorker::Config);
    CONFIG_OBJ(allocate_worker, AllocateWorker::Config);
    CONFIG_OBJ(sync_meta_kv_worker, SyncMetaKvWorker::Config);
    CONFIG_OBJ(forward_client, net::Client::Config);
    CONFIG_OBJ(coroutines_pool_read, DynamicCoroutinesPool::Config);
    CONFIG_OBJ(coroutines_pool_update, DynamicCoroutinesPool::Config);
    CONFIG_OBJ(coroutines_pool_sync, DynamicCoroutinesPool::Config);
    CONFIG_OBJ(coroutines_pool_default, DynamicCoroutinesPool::Config);
    CONFIG_HOT_UPDATED_ITEM(use_coroutines_pool_read, true);
    CONFIG_HOT_UPDATED_ITEM(use_coroutines_pool_update, true);
    CONFIG_HOT_UPDATED_ITEM(speed_up_quit, true);
  };

  Components(const Config &config);

  Result<Void> start(const flat::AppInfo &appInfo, net::ThreadPoolGroup &tpg);
  Result<Void> waitRoutingInfo(const flat::AppInfo &appInfo, folly::CPUThreadPoolExecutor &executor);
  Result<Void> refreshRoutingInfo();
  Result<Void> stopAndJoin(CPUExecutorGroup &executor);
  Result<Void> stopMgmtdClient();
  const flat::AppInfo &getAppInfo() const { return appInfo; }

  Result<robin_hood::unordered_set<std::string>> getActiveClientsList();
  void triggerHeartbeatIfNeed();

  inline DynamicCoroutinesPool &getCoroutinesPool(uint16_t methodId) {
    if (LIKELY(config.use_coroutines_pool_read()) && methodId == StorageSerde<>::batchReadMethodId) {
      return readPool;
    }
    if (LIKELY(config.use_coroutines_pool_update()) &&
        (methodId == StorageSerde<>::writeMethodId || methodId == StorageSerde<>::updateMethodId)) {
      return updatePool;
    }
    if (methodId == StorageSerde<>::syncStartMethodId || methodId == StorageSerde<>::getAllChunkMetadataMethodId) {
      return syncPool;
    }
    return defaultPool;
  }

 protected:
  void updateHeartbeatPayload(const TargetMap &map, bool offline = false);

 public:
  ConstructLog<"storage::Components"> constructLog_;
  const Config &config;
  flat::AppInfo appInfo;
  std::unique_ptr<net::Client> netClient;
  folly::atomic_shared_ptr<hf3fs::client::IMgmtdClientForServer> mgmtdClient;
  BufferPool rdmabufPool;
  AtomicallyTargetMap targetMap;
  StorageTargets storageTargets;
  AioReadWorker aioReadWorker;
  client::StorageMessenger messenger;
  ResyncWorker resyncWorker;
  CheckWorker checkWorker;
  DumpWorker dumpWorker;
  AllocateWorker allocateWorker;
  PunchHoleWorker punchHoleWorker;
  SyncMetaKvWorker syncMetaKvWorker;
  ReliableForwarding reliableForwarding;
  DynamicCoroutinesPool readPool;
  DynamicCoroutinesPool updatePool;
  DynamicCoroutinesPool syncPool;
  DynamicCoroutinesPool defaultPool;
  StorageOperator storageOperator;
  ReliableUpdate reliableUpdate;
  std::atomic<uint32_t> triggerHeartbeatFlag{};
};

}  // namespace hf3fs::storage
