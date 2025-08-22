#include "MgmtdData.h"

#include "MgmtdConfig.h"
#include "core/utils/ServiceOperation.h"
#include "helpers.h"
#include "updateChain.h"

namespace hf3fs::mgmtd {
Result<Void> MgmtdData::checkConfigVersion(core::ServiceOperation &ctx,
                                           flat::NodeType type,
                                           flat::ConfigVersion version) const {
  const auto &cm = configMap;
  auto it = cm.find(type);
  if (it == cm.end()) {
    RETURN_AND_LOG_OP_ERR(ctx, StatusCode::kInvalidArg, "unknown NodeType {}", static_cast<int>(type));
  }
  const auto &versionedMap = it->second;
  auto vit = versionedMap.rbegin();
  if (version > vit->first) {
    RETURN_AND_LOG_OP_ERR(ctx,
                          MgmtdCode::kInvalidConfigVersion,
                          "ConfigVersion {} of {} is newer than server's {}",
                          version,
                          magic_enum::enum_name(type),
                          vit->first);
  }
  return Void{};
}

std::optional<flat::ConfigInfo> MgmtdData::getConfig(flat::NodeType type,
                                                     flat::ConfigVersion version,
                                                     bool latest) const {
  const auto &cm = configMap;
  auto it = cm.find(type);
  assert(it != cm.end());
  const auto &versionedMap = it->second;
  if (latest) {
    auto vit = versionedMap.rbegin();
    const auto &info = vit->second;
    XLOGF_IF(FATAL,
             version > info.configVersion,
             "Should checked that version from user ({}) not larger than from server ({})",
             version.toUnderType(),
             info.configVersion.toUnderType());
    if (version < info.configVersion) {
      return info;
    } else {
      return std::nullopt;
    }
  } else {
    auto vit = versionedMap.find(version);
    if (vit == versionedMap.end()) {
      return std::nullopt;
    } else {
      return vit->second;
    }
  }
}

flat::ConfigVersion MgmtdData::getLatestConfigVersion(flat::NodeType type) const {
  auto it = configMap.find(type);
  assert(it != configMap.end());
  const auto &versionedMap = it->second;
  assert(!versionedMap.empty());
  return versionedMap.rbegin()->first;
}

Result<Void> MgmtdData::checkRoutingInfoVersion(core::ServiceOperation &ctx, flat::RoutingInfoVersion version) const {
  const auto &info = routingInfo;
  if (version > info.routingInfoVersion) {
    RETURN_AND_LOG_OP_ERR(ctx,
                          MgmtdCode::kInvalidRoutingInfoVersion,
                          "RoutingInfoVersion {} is newer than server's ({})",
                          version,
                          info.routingInfoVersion);
  }
  return Void{};
}

std::optional<flat::RoutingInfo> MgmtdData::getRoutingInfo(flat::RoutingInfoVersion version,
                                                           const MgmtdConfig &config) const {
  const auto &info = routingInfo;
  assert(version <= info.routingInfoVersion);
  if (version == info.routingInfoVersion) {
    return std::nullopt;
  }
  bool enableRoutingInfoCache = config.enable_routinginfo_cache();
  if (version != 0 && enableRoutingInfoCache) {
    // version == 0 is possible a force refresh, do not read from cache
    auto cachePtr = routingInfoCache.rlock();
    if (cachePtr->has_value() && cachePtr->value().routingInfoVersion == info.routingInfoVersion) {
      return cachePtr->value();
    }
  }
  flat::RoutingInfo res;
  res.routingInfoVersion = info.routingInfoVersion;
  res.bootstrapping = bootstrapping(config);
  for (const auto &[id, nw] : info.nodeMap) {
    res.nodes[id] = nw.base();
  }
  res.chainTables = info.chainTables;
  res.chains = info.chains;
  for (const auto &[k, v] : info.getTargets()) {
    res.targets[k] = v.base();
  }
  if (enableRoutingInfoCache) {
    // always try to update cache even for a force refresh since the result is reusable to other requests
    auto cachePtr = routingInfoCache.wlock();
    if (!cachePtr->has_value() || cachePtr->value().routingInfoVersion < info.routingInfoVersion) {
      *cachePtr = res;
    }
  }
  return res;
}

void MgmtdData::appendChangedChains(flat::ChainId chainId,
                                    std::vector<flat::ChainInfo> &changedChains,
                                    bool tryAdjustTargetOrder) const {
  if (routingInfo.newBornChains.contains(chainId)) return;

  const auto &oldChain = routingInfo.getChain(chainId);

  std::vector<ChainTargetInfoEx> oldTargets;
  for (const auto &cti : oldChain.targets) {
    oldTargets.emplace_back(cti, routingInfo.getTargets().at(cti.targetId).base().localState);
  }

  auto newTargets = generateNewChain(oldTargets);
  if (tryAdjustTargetOrder && !oldChain.preferredTargetOrder.empty() &&
      newTargets.back().publicState == flat::PublicTargetState::SERVING) {
    newTargets = rotateAsPreferredOrder(newTargets, oldChain.preferredTargetOrder);
  }
  if (oldTargets != newTargets) {
    flat::ChainInfo newChain;
    newChain.chainId = chainId;
    newChain.chainVersion = nextVersion(oldChain.chainVersion);
    newChain.preferredTargetOrder = oldChain.preferredTargetOrder;
    for (const auto &ti : newTargets) {
      newChain.targets.push_back(ti);
    }
    XLOGF(DBG, "append changed chain {}", serde::toJsonString(newChain));
    changedChains.push_back(std::move(newChain));
  }
}

void MgmtdData::reset(flat::RoutingInfoVersion routingInfoVersion,
                      NodeMap allNodes,
                      ConfigMap allConfigs,
                      ChainTableMap allChainTables,
                      ChainMap allChains,
                      TargetMap allTargets,
                      UniversalTagsMap allUniversalTags) {
  routingInfo.reset(routingInfoVersion,
                    std::move(allNodes),
                    std::move(allChainTables),
                    std::move(allChains),
                    std::move(allTargets));
  {
    auto cachePtr = routingInfoCache.wlock();
    cachePtr->reset();
  }

  configMap = std::move(allConfigs);
  lease.bootstrapping = false;
  leaseStartTs = SteadyClock::now();
  universalTagsMap = std::move(allUniversalTags);
}

bool MgmtdData::bootstrapping(const MgmtdConfig &config) const {
  return leaseStartTs + config.bootstrapping_length().asUs() > SteadyClock::now();
}
}  // namespace hf3fs::mgmtd
