#include "storage/service/Components.h"

#include <folly/experimental/coro/BlockingWait.h>

#include "common/app/ApplicationBase.h"
#include "common/monitor/Recorder.h"
#include "common/utils/LogCommands.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"

namespace hf3fs::storage {
namespace {

constexpr std::string_view kRoutingInfoListenerName = "Components";
monitor::ValueRecorder targetStateRecorder{"storage.target_state", std::nullopt, false};

}  // namespace

Components::Components(const Config &config)
    : config(config),
      rdmabufPool(config.buffer_pool()),
      storageTargets(config.targets(), targetMap),
      aioReadWorker(config.aio_read_worker()),
      messenger(config.forward_client()),
      resyncWorker(config.sync_worker(), *this),
      checkWorker(config.check_worker(), *this),
      dumpWorker(config.dump_worker(), *this),
      allocateWorker(config.allocate_worker(), *this),
      punchHoleWorker(*this),
      syncMetaKvWorker(config.sync_meta_kv_worker(), *this),
      reliableForwarding(config.reliable_forwarding(), *this),
      readPool(config.coroutines_pool_read(), "ReadPool"),
      updatePool(config.coroutines_pool_update(), "UpdatePool"),
      syncPool(config.coroutines_pool_default(), "SyncPool"),
      defaultPool(config.coroutines_pool_default(), "DefaultPool"),
      storageOperator(config.storage(), *this),
      reliableUpdate(config.reliable_update(), *this) {}

Result<Void> Components::start(const flat::AppInfo &appInfo, net::ThreadPoolGroup &tpg) {
  this->appInfo = appInfo;

  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start rdmabufPool", rdmabufPool.init(tpg.procThreadPool()));

  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start readPool", readPool.start());
  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start updatePool", updatePool.start());
  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start syncPool", syncPool.start());
  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start defaultPool", defaultPool.start());

  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start messenger", messenger.start());
  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start reliableForwarding", reliableForwarding.init());
  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start storageTargets", storageTargets.load(tpg.procThreadPool()));
  RETURN_ON_ERROR_LOG_WRAPPED(INFO,
                              "Start aioReadWorker",
                              aioReadWorker.start(storageTargets.fds(), rdmabufPool.iovecs()));
  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start dumpWorker", dumpWorker.start(appInfo.nodeId));
  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start allocateWorker", allocateWorker.start());
  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start punchHoleWorker", punchHoleWorker.start());
  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start syncMetaKvWorker", syncMetaKvWorker.start());

  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start waitRoutingInfo", waitRoutingInfo(appInfo, tpg.bgThreadPool().randomPick()));

  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start resyncWorker", resyncWorker.start());
  RETURN_ON_ERROR_LOG_WRAPPED(INFO,
                              "Start checkWorker",
                              checkWorker.start(storageTargets.targetPaths(), storageTargets.manufacturers()));

  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Start storageOperator", storageOperator.init(storageTargets.targetPaths().size()));
  return Void{};
}

Result<Void> Components::waitRoutingInfo(const flat::AppInfo &appInfo, folly::CPUThreadPoolExecutor &executor) {
  // 1. init mgdtd client.
  if (!netClient) {
    netClient = std::make_unique<net::Client>(config.client());
    RETURN_AND_LOG_ON_ERROR(netClient->start());
  }
  if (mgmtdClient.load() == nullptr) {
    auto stubFactory = std::make_unique<hf3fs::stubs::RealStubFactory<mgmtd::MgmtdServiceStub>>(
        hf3fs::stubs::ClientContextCreator{[&](net::Address addr) { return netClient->serdeCtx(addr); }});
    mgmtdClient = std::make_shared<hf3fs::client::MgmtdClientForServer>(appInfo.clusterId,
                                                                        std::move(stubFactory),
                                                                        config.mgmtd());
  }
  mgmtdClient.load()->setAppInfoForHeartbeat(appInfo);
  mgmtdClient.load()->setConfigListener(ApplicationBase::updateConfig);

  // 2. wait target offline.
  auto currentMap = targetMap.snapshot();
  updateHeartbeatPayload(*currentMap, true);
  folly::coro::blockingWait(mgmtdClient.load()->start(&executor));
  for (auto sleep = 0;; ++sleep) {
    if (sleep) {
      XLOGF(WARNING, "Waiting for target offline in routing info...");
      std::this_thread::sleep_for(1000_ms);
    }

    folly::coro::blockingWait(mgmtdClient.load()->heartbeat());
    auto copy = currentMap->clone();
    auto refreshResult = folly::coro::blockingWait(mgmtdClient.load()->refreshRoutingInfo(false));
    if (UNLIKELY(!refreshResult)) {
      XLOGF(ERR, "refresh routing info error {}", refreshResult.error());
      continue;
    }
    auto result = copy->updateRouting(mgmtdClient.load()->getRoutingInfo(), false);
    if (UNLIKELY(!result)) {
      XLOGF(ERR, "get and parse routing info error {}", result.error());
      continue;
    }
    bool needWaiting = false;
    for (auto &[targetId, target] : copy->getTargets()) {
      if (target.publicState == flat::PublicTargetState::SERVING ||
          target.publicState == flat::PublicTargetState::SYNCING ||
          target.publicState == flat::PublicTargetState::WAITING) {
        XLOGF(WARNING, "waiting for chain {} target {}", targetId, serde::toJsonString(target));
        needWaiting = true;
        break;
      }
    }
    if (!needWaiting) {
      break;
    }
  }

  // 3. set listener.
  targetMap.setUpdateCallback([this](const TargetMap &map) { updateHeartbeatPayload(map); });
  RETURN_AND_LOG_ON_ERROR(refreshRoutingInfo());
  folly::coro::blockingWait(mgmtdClient.load()->heartbeat());
  XLOGF(INFO, "Initial target map: {}", serde::toJsonString(targetMap.snapshot()->getTargets()));
  bool succ = mgmtdClient.load()->addRoutingInfoListener(std::string{kRoutingInfoListenerName},
                                                         [this](auto) { refreshRoutingInfo(); });
  if (UNLIKELY(!succ)) {
    auto msg = fmt::format("node {} addRoutingInfoListener failed!", appInfo.nodeId);
    XLOG(ERR, msg);
    return makeError(StorageCode::kStorageInitFailed, std::move(msg));
  }
  return Void{};
}

Result<Void> Components::refreshRoutingInfo() { return targetMap.updateRouting(mgmtdClient.load()->getRoutingInfo()); }

Result<Void> Components::stopAndJoin(CPUExecutorGroup &executor) {
  LOG_COMMAND(INFO, "Stop aioReadWorker", aioReadWorker.stopAndJoin());
  LOG_COMMAND(INFO, "Stop syncMetaKvWorker", syncMetaKvWorker.stopAndJoin());
  LOG_COMMAND(INFO, "Stop punchHoleWorker", punchHoleWorker.stopAndJoin());
  LOG_COMMAND(INFO, "Stop allocateWorker", allocateWorker.stopAndJoin());
  LOG_COMMAND(INFO, "Stop dumpWorker", dumpWorker.stopAndJoin());
  LOG_COMMAND(INFO, "Stop checkWorker", checkWorker.stopAndJoin());
  LOG_COMMAND(INFO, "Stop resyncWorker", resyncWorker.stopAndJoin());
  LOG_COMMAND(INFO, "Stop storageOperator", storageOperator.stopAndJoin());
  LOG_COMMAND(INFO, "Stop reliableForwarding", reliableForwarding.stopAndJoin());
  LOG_COMMAND(INFO, "Stop messenger", messenger.stopAndJoin());
  targetMap.setUpdateCallback([](auto) {});
  XLOGF(INFO, "Send offline state");
  if (auto mgmtd = mgmtdClient.load()) {
    mgmtd->removeRoutingInfoListener(kRoutingInfoListenerName);
    updateHeartbeatPayload(*targetMap.snapshot(), true);
    folly::coro::blockingWait(mgmtd->heartbeat());
  }
  LOG_COMMAND(INFO, "Stop routingStore", stopMgmtdClient());

  LOG_COMMAND(INFO, "Stop readPool", readPool.stopAndJoin());
  LOG_COMMAND(INFO, "Stop updatePool", updatePool.stopAndJoin());
  LOG_COMMAND(INFO, "Stop syncPool", syncPool.stopAndJoin());
  LOG_COMMAND(INFO, "Stop defaultPool", defaultPool.stopAndJoin());

  auto snapshot = targetMap.release();
  std::vector<std::shared_ptr<StorageTarget>> targets;
  for (auto &[targetId, target] : snapshot->getTargets()) {
    if (target.storageTarget != nullptr) {
      targets.push_back(target.storageTarget);
    }
  }
  LOG_COMMAND(INFO, "Reset target map", snapshot.reset());

  XLOGF(WARNING, "start to release {} targets", targets.size());
  std::atomic<uint32_t> released{};
  std::atomic<uint32_t> synced{};
  for (auto &target : targets) {
    executor.randomPick().add([&, t = std::move(target)]() mutable {
      auto result = t->release();
      if (UNLIKELY(!result)) {
        XLOGF(CRITICAL, "storage target sync meta failed {}, error: {}", t->path(), result.error());
      } else {
        ++synced;
      }
      t = nullptr;
      ++released;
    });
  }

  for (int i = 0; released != targets.size(); ++i) {
    XLOGF_IF(INFO, i % 5 == 0, "Waiting for release targets finished...");
    std::this_thread::sleep_for(100_ms);
  }

  XLOGF(WARNING, "released {} targets, synced {} targets", released.load(), synced.load());

  LOG_COMMAND(INFO, "Clear storageTargets", storageTargets.globalFileStore().clear(executor));
  LOG_COMMAND(INFO, "Clear rdmabufPool", rdmabufPool.clear(executor));
  if (config.speed_up_quit()) {
    for (auto &engine : storageTargets.engines()) {
      engine->speed_up_quit();
    }
  }
  return Void{};
}

Result<Void> Components::stopMgmtdClient() {
  if (mgmtdClient.load()) {
    folly::coro::blockingWait(mgmtdClient.load()->stop());
  }
  mgmtdClient.store(nullptr);
  if (netClient) {
    netClient->stopAndJoin();
  }
  netClient.reset();
  return Void{};
}

Result<robin_hood::unordered_set<std::string>> Components::getActiveClientsList() {
  auto result = folly::coro::blockingWait(mgmtdClient.load()->listClientSessions());
  RETURN_AND_LOG_ON_ERROR(result);
  if (result->bootstrapping) {
    auto msg = fmt::format("mgmtd is bootstrapping, skip");
    XLOG(WARNING, msg);
    return makeError(StorageClientCode::kRoutingError, std::move(msg));
  }

  robin_hood::unordered_set<std::string> activeClients;
  for (auto &client : result->sessions) {
    activeClients.emplace(std::move(client.clientId));
  }
  return Result<robin_hood::unordered_set<std::string>>(std::move(activeClients));
}

void Components::triggerHeartbeatIfNeed() {
  if (triggerHeartbeatFlag.exchange(0)) {
    mgmtdClient.load()->triggerHeartbeat();
  }
}

void Components::updateHeartbeatPayload(const TargetMap &targetMap, bool offline /* = false */) {
  flat::StorageHeartbeatInfo heartbeat;
  for (auto &[targetId, target] : targetMap.getTargets()) {
    flat::LocalTargetInfo targetInfo;
    targetInfo.targetId = targetId;
    targetInfo.localState = offline ? flat::LocalTargetState::OFFLINE : target.localState;
    targetInfo.diskIndex = target.diskIndex;
    targetInfo.lowSpace = target.lowSpace;
    monitor::TagSet tag;
    tag.addTag("instance", fmt::format("{}", targetId));
    targetStateRecorder.set(uint32_t(target.localState), tag);
    if (targetInfo.localState != flat::LocalTargetState::OFFLINE) {
      targetInfo.usedSize = target.storageTarget->usedSize();
      targetInfo.chainVersion = target.vChainId.chainVer;
    }
    heartbeat.targets.push_back(targetInfo);
  }
  mgmtdClient.load()->updateHeartbeatPayload(heartbeat);
  ++triggerHeartbeatFlag;
}

}  // namespace hf3fs::storage
