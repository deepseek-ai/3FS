#pragma once

#include "LocalTargetInfoWithNodeId.h"
#include "NodeInfoWrapper.h"
#include "TargetInfo.h"
#include "fbs/mgmtd/ChainInfo.h"
#include "fbs/mgmtd/ChainTable.h"
#include "fbs/mgmtd/MgmtdTypes.h"

namespace hf3fs::core {
struct ServiceOperation;
}

namespace hf3fs::mgmtd {
struct MgmtdConfig;
class MgmtdTargetInfoLoader;

namespace testing {
class MgmtdTestHelper;
}

using NodeMap = robin_hood::unordered_map<flat::NodeId, NodeInfoWrapper>;
using ChainTableVersionMap = std::map<flat::ChainTableVersion, flat::ChainTable>;
using ChainTableMap = robin_hood::unordered_map<flat::ChainTableId, ChainTableVersionMap>;
using ChainMap = robin_hood::unordered_map<flat::ChainId, flat::ChainInfo>;
using TargetMap = robin_hood::unordered_map<flat::TargetId, TargetInfo>;
using NewBornChains = robin_hood::unordered_map<flat::ChainId, SteadyTime>;
using OrphanTargetsByTargetId = robin_hood::unordered_map<flat::TargetId, flat::TargetInfo>;
using OrphanTargetsByNodeId = robin_hood::unordered_map<flat::NodeId, robin_hood::unordered_set<flat::TargetId>>;

struct RoutingInfo {
  bool routingInfoChanged = false;                  // periodically promote routingInfoVersion
  flat::RoutingInfoVersion routingInfoVersion{1};   // ensure version 0 is less than any valid version
  NodeMap nodeMap;                                  // persistent
  ChainTableMap chainTables;                        // persistent
  ChainMap chains;                                  // persistent
  NewBornChains newBornChains;                      // temporal
  OrphanTargetsByTargetId orphanTargetsByTargetId;  // temporal
  OrphanTargetsByNodeId orphanTargetsByNodeId;      // temporal

  void applyChainTargetChanges(core::ServiceOperation &ctx,
                               const std::vector<flat::ChainInfo> &changedChains,
                               SteadyTime now);

  void localUpdateTargets(flat::NodeId nodeId,
                          const std::vector<flat::LocalTargetInfo> &targets,
                          const MgmtdConfig &config);

  flat::ChainInfo &getChain(flat::ChainId cid);
  const flat::ChainInfo &getChain(flat::ChainId cid) const;

  void insertNewChain(const flat::ChainInfo &chain);

  void insertNewTarget(flat::ChainId cid, const flat::ChainTargetInfo &cti);

  void removeTarget(flat::ChainId cid, flat::TargetId tid);

  const TargetMap &getTargets() const { return targets; }

  auto updateTarget(flat::TargetId tid, auto &&func) {
    XLOGF_IF(DFATAL, !targets.contains(tid), "tid = {}", tid);
    auto &ti = targets[tid];
    return func(ti);
  }

  void reset(flat::RoutingInfoVersion routingInfoVersion,
             NodeMap allNodes,
             ChainTableMap allChainTables,
             ChainMap allChains,
             TargetMap allTargets);

 private:
  void eraseOrphanTarget(flat::TargetId tid);

  TargetMap targets;  // derived

  friend class testing::MgmtdTestHelper;
};

}  // namespace hf3fs::mgmtd
