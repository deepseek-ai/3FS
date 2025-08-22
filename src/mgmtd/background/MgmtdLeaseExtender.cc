#include "MgmtdLeaseExtender.h"

#include <folly/experimental/coro/Collect.h>

#include "common/utils/StringUtils.h"
#include "core/utils/ServiceOperation.h"
#include "core/utils/runOp.h"
#include "fbs/mgmtd/NodeConversion.h"
#include "mgmtd/service/MgmtdState.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {
void addNode(NodeMap &allNodes, flat::NodeInfo nodeInfo) {
  auto &node = allNodes[nodeInfo.app.nodeId];
  node.base() = std::move(nodeInfo);
  node.recordStatusChange(node.base().status, UtcClock::now());
}

CoTryTask<void> loadRoutingInfoVersion(core::ServiceOperation &ctx,
                                       MgmtdStore &store,
                                       kv::IReadWriteTransaction &txn,
                                       flat::RoutingInfoVersion &newVersion) {
  auto res = co_await store.loadRoutingInfoVersion(txn);
  CO_RETURN_ON_ERROR(res);
  LOG_OP_INFO(ctx, "Load routingInfoVersion {}", *res);
  newVersion = nextVersion(*res);
  co_return Void{};
}

CoTryTask<void> persistNewVersionAndSelfInfo(core::ServiceOperation &ctx,
                                             MgmtdStore &store,
                                             kv::IReadWriteTransaction &txn,
                                             flat::NodeInfo &selfNodeInfo,
                                             flat::PersistentNodeInfo &selfPersistentNodeInfo,
                                             flat::RoutingInfoVersion newVersion,
                                             NodeMap &allNodes) {
  CO_RETURN_ON_ERROR(co_await store.storeRoutingInfoVersion(txn, newVersion));
  bool needPersistSelf = true;
  auto it = allNodes.find(selfNodeInfo.app.nodeId);
  if (it != allNodes.end()) {
    XLOGF_IF(FATAL,
             it->second.base().type != flat::NodeType::MGMTD,
             "Unexpected self type: {}",
             toString(it->second.base().type));
    auto persisted = flat::toPersistentNode(it->second.base());
    selfNodeInfo.tags = persisted.tags;
    selfPersistentNodeInfo.tags = persisted.tags;
    if (persisted == selfPersistentNodeInfo) {
      needPersistSelf = false;
    }
  }
  if (needPersistSelf) {
    CO_RETURN_ON_ERROR(co_await store.storeNodeInfo(txn, selfPersistentNodeInfo));
  }
  allNodes[selfNodeInfo.app.nodeId].base() = selfNodeInfo;
  co_return Void{};
}

CoTryTask<void> loadAllNodes(core::ServiceOperation &ctx,
                             MgmtdStore &store,
                             kv::IReadWriteTransaction &txn,
                             NodeMap &allNodes) {
  auto loadRes = co_await store.loadAllNodes(txn);
  CO_RETURN_ON_ERROR(loadRes);
  LOG_OP_INFO(ctx, "Load {} nodes", loadRes->size());
  for (auto &sn : *loadRes) {
    addNode(allNodes, flat::toNode(sn));
  }
  co_return Void{};
}

CoTryTask<void> loadAllConfigs(core::ServiceOperation &ctx,
                               MgmtdStore &store,
                               kv::IReadOnlyTransaction &txn,
                               ConfigMap &configMap) {
  auto loadRes = co_await store.loadAllConfigs(txn);
  CO_RETURN_ON_ERROR(loadRes);
  for (auto &[type, info] : *loadRes) {
    LOG_OP_INFO(ctx, "Load config {} version {}", magic_enum::enum_name(type), info.configVersion);
    auto ver = info.configVersion;
    configMap[type][ver] = std::move(info);
  }
  co_return Void{};
}

CoTryTask<void> loadAllChainTables(core::ServiceOperation &ctx,
                                   MgmtdStore &store,
                                   kv::IReadOnlyTransaction &txn,
                                   ChainTableMap &chainTables) {
  auto loadRes = co_await store.loadAllChainTables(txn);
  CO_RETURN_ON_ERROR(loadRes);
  for (auto &info : *loadRes) {
    chainTables[info.chainTableId][info.chainTableVersion] = info;
  }
  co_return Void{};
}

CoTryTask<void> loadAllChains(core::ServiceOperation &ctx,
                              MgmtdStore &store,
                              kv::IReadOnlyTransaction &txn,
                              ChainMap &chains,
                              TargetMap &targetMap) {
  auto loadRes = co_await store.loadAllChains(txn);
  CO_RETURN_ON_ERROR(loadRes);
  auto steadyNow = SteadyClock::now();
  for (auto &info : *loadRes) {
    for (const auto &t : info.targets) {
      targetMap[t.targetId] = TargetInfo(makeTargetInfo(info.chainId, t), steadyNow);
    }
    chains[info.chainId] = info;
  }
  co_return Void{};
}

CoTryTask<void> loadAllUniversalTags(core::ServiceOperation &ctx,
                                     MgmtdStore &store,
                                     kv::IReadOnlyTransaction &txn,
                                     UniversalTagsMap &universalTagsMap) {
  auto loadRes = co_await store.loadAllUniversalTags(txn);
  CO_RETURN_ON_ERROR(loadRes);
  for (auto &[id, tags] : *loadRes) {
    universalTagsMap[id] = std::move(tags);
  }
  co_return Void{};
}

CoTryTask<void> onNewLease(MgmtdState &state, core::ServiceOperation &ctx, MgmtdData &data, UtcTime start) {
  flat::RoutingInfoVersion newRoutingInfoVersion{1};
  NodeMap allNodes;
  ConfigMap allConfigs;
  magic_enum::enum_for_each<flat::NodeType>(
      [&](auto type) { allConfigs[type][flat::ConfigVersion(0)] = flat::ConfigInfo{}; });
  ChainTableMap chainTables;
  ChainMap chains;
  TargetMap targetMap;
  UniversalTagsMap universalTagsMap;

  auto handler = [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
    auto [rivRes, nodesRes, configsRes, chainTablesRes, chainsRes, utagsRes] =
        co_await folly::coro::collectAll(loadRoutingInfoVersion(ctx, state.store_, txn, newRoutingInfoVersion),
                                         loadAllNodes(ctx, state.store_, txn, allNodes),
                                         loadAllConfigs(ctx, state.store_, txn, allConfigs),
                                         loadAllChainTables(ctx, state.store_, txn, chainTables),
                                         loadAllChains(ctx, state.store_, txn, chains, targetMap),
                                         loadAllUniversalTags(ctx, state.store_, txn, universalTagsMap));
    CO_RETURN_ON_ERROR(rivRes);
    CO_RETURN_ON_ERROR(nodesRes);
    CO_RETURN_ON_ERROR(configsRes);
    CO_RETURN_ON_ERROR(chainTablesRes);
    CO_RETURN_ON_ERROR(chainsRes);
    CO_RETURN_ON_ERROR(utagsRes);

    // TODO: check routingInfo integrity

    co_return co_await persistNewVersionAndSelfInfo(ctx,
                                                    state.store_,
                                                    txn,
                                                    state.selfNodeInfo_,
                                                    state.selfPersistentNodeInfo_,
                                                    newRoutingInfoVersion,
                                                    allNodes);
  };
  auto loadRes = co_await withReadWriteTxn(state, std::move(handler));

  if (!loadRes.hasError()) {
    XLOGF_IF(FATAL, !allConfigs.contains(flat::NodeType::MGMTD), "Found no config for MGMTD");
    const auto &cfg = allConfigs[flat::NodeType::MGMTD].rbegin()->second;
    if (cfg.configVersion && updateSelfConfig(state, cfg)) {
      state.selfNodeInfo_.configVersion = cfg.configVersion;
      allNodes[state.selfId()].base().configVersion = cfg.configVersion;
    }
    data.reset(newRoutingInfoVersion,
               std::move(allNodes),
               std::move(allConfigs),
               std::move(chainTables),
               std::move(chains),
               std::move(targetMap),
               std::move(universalTagsMap));

    auto clientSessionMap = co_await state.clientSessionMap_.coLock();
    clientSessionMap->clear();
    co_return Void{};
  } else {
    // retry in next round
    CO_RETURN_ERROR(loadRes);
  }
}

struct Op : core::ServiceOperationWithMetric<"MgmtdService", "ExtendLease", "bg"> {
  String toStringImpl() const final { return "ExtendLease"; }

  auto handle(MgmtdState &state) -> CoTryTask<void> {
    auto handler = [&](kv::IReadWriteTransaction &txn) -> CoTryTask<std::optional<flat::MgmtdLeaseInfo>> {
      auto checkSelfRes = co_await state.store_.loadNodeInfo(txn, state.selfId());
      CO_RETURN_ON_ERROR(checkSelfRes);

      if (checkSelfRes->has_value() && flat::findTag(checkSelfRes->value().tags, flat::kDisabledTagKey) != -1) {
        co_return std::nullopt;
      }

      auto extendRes = co_await state.store_.extendLease(txn,
                                                         state.selfPersistentNodeInfo_,
                                                         state.config_.lease_length().asUs(),
                                                         state.utcNow(),
                                                         flat::ReleaseVersion::fromVersionInfo(),
                                                         state.config_.extend_lease_check_release_version());
      CO_RETURN_ON_ERROR(extendRes);
      co_return *extendRes;
    };

    auto commitRes = co_await withReadWriteTxn(state, std::move(handler), /*expectSelfPrimary=*/false);
    if (commitRes.hasError()) {
      co_return makeError(commitRes.error().code(), fmt::format("failed to commit: {}", commitRes.error().message()));
    }

    auto writerLock = co_await state.coScopedLock<"ExtendLease">();
    auto start = state.utcNow();

    auto dataPtr = co_await state.data_.coLock();

    if (!commitRes->has_value()) {
      LOG_OP_INFO(*this, "self is disabled, release lease");
      dataPtr->lease.lease.reset();
      co_return Void{};
    }

    const auto &newLease = commitRes->value();
    auto &leaseInfo = dataPtr->lease;
    String leaseChangedReason = [&] {
      if (!leaseInfo.lease.has_value()) return "prev no lease";
      if (leaseInfo.lease->primary.nodeId != newLease.primary.nodeId) return "primary id changed";
      if (leaseInfo.lease->leaseStart != newLease.leaseStart) return "lease start changed";
      if (leaseInfo.bootstrapping) return "still bootstrapping";
      return "";
    }();

    leaseInfo.lease = newLease;
    leaseInfo.bootstrapping = newLease.primary.nodeId == state.selfId() && !leaseChangedReason.empty();

    if (leaseChangedReason.empty()) {
      LOG_OP_DBG(*this, "lease not changed");
    } else {
      LOG_OP_INFO(*this,
                  "newLease({}-{}-{}) changed({})",
                  newLease.primary.nodeId,
                  newLease.leaseStart.toMicroseconds(),
                  newLease.leaseEnd.toMicroseconds(),
                  leaseChangedReason);
    }

    if (leaseInfo.bootstrapping) {
      LOG_OP_INFO(*this, "self got lease, loading ...");
      co_return co_await onNewLease(state, *this, *dataPtr, start);
    }

    co_return Void{};
  }
};
}  // namespace
MgmtdLeaseExtender::MgmtdLeaseExtender(MgmtdState &state)
    : state_(state) {}

CoTask<void> MgmtdLeaseExtender::extend() {
  Op op;

  co_await [&]() -> CoTryTask<void> { CO_INVOKE_OP_INFO(op, "background", state_); }();
}
}  // namespace hf3fs::mgmtd
