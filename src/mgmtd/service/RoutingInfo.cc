#include "RoutingInfo.h"

#include "MgmtdConfig.h"
#include "core/utils/ServiceOperation.h"
#include "helpers.h"

namespace hf3fs::mgmtd {
void RoutingInfo::applyChainTargetChanges(core::ServiceOperation &ctx,
                                          const std::vector<flat::ChainInfo> &changedChains,
                                          SteadyTime now) {
  for (const auto &chain : changedChains) {
    auto &oldChain = getChain(chain.chainId);
    LOG_OP_INFO(ctx,
                "{} change from {} to {}",
                chain.chainId,
                serde::toJsonString(oldChain),
                serde::toJsonString(chain));
    oldChain = chain;
    for (const auto &cti : chain.targets) {
      updateTarget(cti.targetId, [&](auto &ti) {
        ti.base().publicState = cti.publicState;
        ti.updateTs(now);
      });
    }
  }
}

flat::ChainInfo &RoutingInfo::getChain(flat::ChainId cid) {
  XLOGF_IF(FATAL, !chains.contains(cid), "cid = {}", cid.toUnderType());
  return chains[cid];
}

const flat::ChainInfo &RoutingInfo::getChain(flat::ChainId cid) const {
  XLOGF_IF(FATAL, !chains.contains(cid), "cid = {}", cid.toUnderType());
  return chains.at(cid);
}

void RoutingInfo::localUpdateTargets(flat::NodeId nodeId,
                                     const std::vector<flat::LocalTargetInfo> &targets,
                                     const MgmtdConfig &config) {
  auto steadyNow = SteadyClock::now();

  auto &orphans = orphanTargetsByNodeId[nodeId];
  for (auto tid : orphans) {
    orphanTargetsByTargetId.erase(tid);
  }
  orphans.clear();

  for (const auto &lti : targets) {
    auto it = this->targets.find(lti.targetId);
    if (it != this->targets.end()) {
      updateTarget(lti.targetId, [&](auto &ti) {
        auto &base = ti.base();

        bool shouldIgnore = [&] {
          if (lti.chainVersion == 0 || !config.heartbeat_ignore_stale_targets()) return false;
          const auto &chain = getChain(base.chainId);
          return chain.chainVersion > lti.chainVersion;
        }();

        if (shouldIgnore) return;

        ti.updateTs(steadyNow);

        bool importantInfoChanged = base.localState != lti.localState || base.nodeId != nodeId;
        if (importantInfoChanged) {
          ti.importantInfoChangedTime = steadyNow;
        }

        base.localState = lti.localState;
        base.nodeId = nodeId;
        base.diskIndex = lti.diskIndex;
        base.usedSize = lti.usedSize;
      });
    } else {
      // only insert to orphans, not targets
      orphans.insert(lti.targetId);
      auto &ti = orphanTargetsByTargetId[lti.targetId];
      ti = flat::TargetInfo();  // ensure clean state
      ti.targetId = lti.targetId;
      ti.localState = lti.localState;
      ti.nodeId = nodeId;
      ti.diskIndex = lti.diskIndex;
      ti.usedSize = lti.usedSize;
    }
  }
}

void RoutingInfo::insertNewChain(const flat::ChainInfo &chain) {
  auto cid = chain.chainId;
  XLOGF_IF(FATAL, chains.contains(cid), "Insert duplicated chain {}", cid.toUnderType());
  auto createdTime = SteadyClock::now();
  chains[cid] = chain;
  for (const auto &t : chain.targets) {
    auto tid = t.targetId;
    XLOGF_IF(FATAL, targets.contains(tid), "Insert duplicated target {}", tid.toUnderType());
    targets[tid] = TargetInfo(makeTargetInfo(cid, t), createdTime);
    eraseOrphanTarget(tid);
  }
  newBornChains[cid] = createdTime;
}

void RoutingInfo::insertNewTarget(flat::ChainId cid, const flat::ChainTargetInfo &cti) {
  auto tid = cti.targetId;
  XLOGF_IF(FATAL, targets.contains(tid), "Insert duplicated target {}", tid.toUnderType());
  targets[tid] = TargetInfo(makeTargetInfo(cid, cti), SteadyClock::now());
  eraseOrphanTarget(tid);
}

void RoutingInfo::removeTarget(flat::ChainId cid, flat::TargetId tid) {
  XLOGF_IF(FATAL, !targets.contains(tid), "Try to remove unknown target {}", tid.toUnderType());
  auto &ti = targets[tid];
  XLOGF_IF(FATAL,
           ti.base().publicState != flat::PublicTargetState::OFFLINE,
           "Try to remove target {} which is not offlined",
           tid.toUnderType());
  if (chains.contains(cid)) {
    auto &chain = getChain(cid);
    XLOGF_IF(
        FATAL,
        std::find_if(chain.targets.begin(), chain.targets.end(), [&](const auto &ti) { return ti.targetId == tid; }) !=
            chain.targets.end(),
        "target {} is still present in chain {}",
        tid.toUnderType(),
        cid.toUnderType());
  }
  targets.erase(tid);
}

void RoutingInfo::eraseOrphanTarget(flat::TargetId tid) {
  if (LIKELY(!orphanTargetsByTargetId.contains(tid))) return;
  const auto &oti = orphanTargetsByTargetId[tid];
  XLOGF_IF(FATAL, !oti.nodeId.has_value(), "Unexpected orphan target: {}", serde::toJsonString(oti));
  XLOGF_IF(FATAL,
           !orphanTargetsByNodeId.contains(*oti.nodeId),
           "Unexpected orphan target ref: {}",
           serde::toJsonString(oti));
  auto &set = orphanTargetsByNodeId[*oti.nodeId];
  XLOGF_IF(FATAL, !set.contains(tid), "Unexpected orphan target ref: {}", serde::toJsonString(oti));
  set.erase(tid);
  if (set.empty()) orphanTargetsByNodeId.erase(*oti.nodeId);
  orphanTargetsByTargetId.erase(tid);
}

void RoutingInfo::reset(flat::RoutingInfoVersion routingInfoVersion,
                        NodeMap allNodes,
                        ChainTableMap allChainTables,
                        ChainMap allChains,
                        TargetMap allTargets) {
  XLOGF(INFO, "Reset RoutingInfo to routingInfoVersion {}", routingInfoVersion);
  auto steadyNow = SteadyClock::now();

  nodeMap.clear();
  chainTables.clear();
  chains.clear();
  newBornChains.clear();
  orphanTargetsByTargetId.clear();
  orphanTargetsByNodeId.clear();

  for (const auto &[id, sn] : allNodes) {
    nodeMap.try_emplace(id, sn.base(), steadyNow);
  }
  chainTables = std::move(allChainTables);
  chains = std::move(allChains);
  targets = std::move(allTargets);
  this->routingInfoVersion = routingInfoVersion;

  for ([[maybe_unused]] const auto &[cid, _] : chains) {
    // avoid update chain too early after restart or the chain status may be not stable
    newBornChains[cid] = steadyNow;
  }
}
}  // namespace hf3fs::mgmtd
