#pragma once

#include <folly/Synchronized.h>

#include "ClientSession.h"
#include "LeaseInfo.h"
#include "RoutingInfo.h"
#include "common/utils/RobinHoodUtils.h"
#include "fbs/mgmtd/ConfigInfo.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "fbs/mgmtd/RoutingInfo.h"

namespace hf3fs::core {
struct ServiceOperation;
}

namespace hf3fs::mgmtd {
using VersionedConfigMap = std::map<flat::ConfigVersion, flat::ConfigInfo>;
using ConfigMap = std::map<flat::NodeType, VersionedConfigMap>;
using UniversalTagsMap = RHStringHashMap<std::vector<flat::TagPair>>;

struct MgmtdConfig;

struct MgmtdData {
  SteadyTime leaseStartTs;
  RoutingInfo routingInfo;
  ConfigMap configMap;
  LeaseInfo lease;
  UniversalTagsMap universalTagsMap;

  mutable folly::Synchronized<std::optional<flat::RoutingInfo>> routingInfoCache;

  Result<Void> checkConfigVersion(core::ServiceOperation &ctx, flat::NodeType, flat::ConfigVersion version) const;
  std::optional<flat::ConfigInfo> getConfig(flat::NodeType, flat::ConfigVersion version, bool latest) const;
  flat::ConfigVersion getLatestConfigVersion(flat::NodeType) const;

  Result<Void> checkRoutingInfoVersion(core::ServiceOperation &ctx, flat::RoutingInfoVersion version) const;
  std::optional<flat::RoutingInfo> getRoutingInfo(flat::RoutingInfoVersion version, const MgmtdConfig &config) const;

  void appendChangedChains(flat::ChainId chainId,
                           std::vector<flat::ChainInfo> &changedChains,
                           bool tryAdjustTargetOrder) const;

  void reset(flat::RoutingInfoVersion routingInfoVersion,
             NodeMap allNodes,
             ConfigMap allConfigs,
             ChainTableMap allChainTables,
             ChainMap allChains,
             TargetMap allTargets,
             UniversalTagsMap allUniversalTags);

  bool bootstrapping(const MgmtdConfig &config) const;
};

}  // namespace hf3fs::mgmtd
