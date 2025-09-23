#include "storage/service/TargetMap.h"

#include <algorithm>

#include "common/monitor/Recorder.h"
#include "common/utils/RobinHood.h"
#include "fbs/mgmtd/MgmtdTypes.h"

namespace hf3fs::storage {
namespace {

monitor::OperationRecorder updateRoutingRecorder{"storage.update_routing"};

}  // namespace

Result<net::Address> Target::getSuccessorAddr() const {
  if (UNLIKELY(!successor.has_value())) {
    return makeError(StorageCode::kNoSuccessorTarget);
  }
  auto &serviceGroups = successor->nodeInfo.app.serviceGroups;
  if (UNLIKELY(serviceGroups.empty())) {
    auto msg = fmt::format("target {} successor service groups is empty", *this);
    XLOG(ERR, msg);
    return makeError(StorageCode::kNoSuccessorAddr, std::move(msg));
  }
  auto &endpoints = serviceGroups.front().endpoints;
  if (UNLIKELY(endpoints.empty())) {
    auto msg = fmt::format("target {} successor service endpoints is empty", *this);
    XLOG(ERR, msg);
    return makeError(StorageCode::kNoSuccessorAddr, std::move(msg));
  }
  return endpoints.front();
}

Result<TargetId> TargetMap::getTargetId(ChainId chainId) const {
  auto chainToTargetIt = chainToTarget_.find(chainId);
  if (UNLIKELY(chainToTargetIt == chainToTarget_.end())) {
    auto msg = fmt::format("chain {} not found", chainId);
    XLOG(ERR, msg);
    return makeError(StorageClientCode::kRoutingError, std::move(msg));
  }
  return chainToTargetIt->second;
}

Result<const Target *> TargetMap::getTarget(TargetId targetId) const {
  auto targetsIt = targets_.find(targetId);
  if (UNLIKELY(targetsIt == targets_.end())) {
    auto msg = fmt::format("target {} not found", targetId);
    XLOG(ERR, msg);
    return makeError(StorageClientCode::kRoutingError, std::move(msg));
  }
  return &targetsIt->second;
}

Result<const Target *> TargetMap::getByChainId(VersionedChainId vChainId, bool allowOutdatedChainVer) const {
  CHECK_RESULT(targetId, getTargetId(vChainId.chainId));
  CHECK_RESULT(target, getTarget(targetId));
  if (target->vChainId != vChainId && !allowOutdatedChainVer) {
    auto msg = fmt::format("chain {} version mismatch request {} != local {}",
                           vChainId.chainId,
                           vChainId.chainVer,
                           target->vChainId.chainVer);
    XLOG(ERR, msg);
    return makeError(StorageClientCode::kRoutingVersionMismatch, std::move(msg));
  }
  if (target->localState == flat::LocalTargetState::OFFLINE) {
    auto msg = fmt::format("chain {} target {} is offline", vChainId.chainId, target->targetId);
    XLOG(ERR, msg);
    return makeError(StorageCode::kTargetOffline, std::move(msg));
  }
  if (target->storageTarget == nullptr) {
    auto msg = fmt::format("chain {} target {} is offline", vChainId.chainId, target->targetId);
    XLOG(CRITICAL, msg);
    return makeError(StorageCode::kTargetOffline, std::move(msg));
  }
  return target;
}

Result<Void> TargetMap::addStorageTarget(const std::shared_ptr<StorageTarget> &storageTarget) {
  auto targetId = storageTarget->targetId();
  Target target;
  target.storageTarget = storageTarget;
  target.targetId = targetId;
  target.chainId = storageTarget->chainId();
  target.path = storageTarget->path();
  target.localState = flat::LocalTargetState::ONLINE;
  target.diskIndex = storageTarget->diskIndex();
  target.useChunkEngine = storageTarget->useChunkEngine();
  auto [it, succ] = targets_.emplace(targetId, target);
  if (UNLIKELY(!succ)) {
    if (it->second.localState == flat::LocalTargetState::OFFLINE) {
      it->second = std::move(target);
      return Void{};
    }
    auto msg = fmt::format("target {} already exists", targetId);
    XLOG(ERR, msg);
    return makeError(StorageCode::kTargetStateInvalid, std::move(msg));
  }
  return Void{};
}

Result<Target *> TargetMap::getMutableTarget(TargetId targetId) {
  auto targetsIt = targets_.find(targetId);
  if (UNLIKELY(targetsIt == targets_.end())) {
    auto msg = fmt::format("target {} not found", targetId);
    return makeError(StorageClientCode::kRoutingError, std::move(msg));
  }
  return &targetsIt->second;
}

Result<Void> TargetMap::syncReceiveDone(VersionedChainId chainId) {
  CHECK_RESULT(constTarget, getByChainId(chainId, false));
  auto targetId = constTarget->targetId;
  CHECK_RESULT(target, getMutableTarget(targetId));
  XLOGF(WARNING,
        "chain {} target {} sync receive done {} -> UPTODATE",
        chainId,
        targetId,
        magic_enum::enum_name(target->localState));
  target->localState = flat::LocalTargetState::UPTODATE;
  return Void{};
}

Result<Void> TargetMap::updateRouting(std::shared_ptr<hf3fs::client::RoutingInfo> r, bool log /* = true */) {
  auto recordGuard = updateRoutingRecorder.record();

  if (UNLIKELY(r == nullptr)) {
    XLOGF(ERR, "routing info is empty");
    return makeError(StorageClientCode::kRoutingError, "routing info is empty");
  }
  auto &routingInfo = r->raw();
  if (routingInfoVersion_ > routingInfo->routingInfoVersion) {
    auto msg = fmt::format("routing info expired! {} > {}", routingInfoVersion_, routingInfo->routingInfoVersion);
    XLOG(ERR, msg);
    return makeError(StorageClientCode::kRoutingError, std::move(msg));
  }
  XLOGF(INFO, "routing info updated, {} -> {}", routingInfoVersion_, routingInfo->routingInfoVersion);

  // 1. reset current state.
  routingInfoVersion_ = routingInfo->routingInfoVersion;
  chainToTarget_.clear();
  syncingChains_.clear();
  robin_hood::unordered_set<TargetId> headTargets;
  robin_hood::unordered_set<TargetId> tailTargets;
  robin_hood::unordered_set<TargetId> lastSrvTargets;
  for (auto &[targetId, target] : targets_) {
    if (target.isHead) {
      headTargets.insert(target.targetId);
    }
    if (target.isTail) {
      tailTargets.insert(target.targetId);
    }
    if (target.publicState == flat::PublicTargetState::LASTSRV) {
      lastSrvTargets.insert(target.targetId);
    }
    target.isHead = false;
    target.isTail = false;
    target.vChainId = VersionedChainId{};
    target.publicState = flat::PublicTargetState::INVALID;
    target.successor = std::nullopt;
  }
  bool invalidRoutingInfo = false;
  auto invalidRoutingInfoLogGuard = folly::makeGuard([&] {
    if (invalidRoutingInfo) {
      XLOGF(CRITICAL, "invalid routing info: {}", *routingInfo);
    }
  });

  // 2. iterate routing info.
  for (auto &[id, chain] : routingInfo->chains) {
    // 3. find target in chain.
    auto it = std::find_if(chain.targets.begin(), chain.targets.end(), [&](const flat::ChainTargetInfo &targetInfo) {
      return bool(getMutableTarget(targetInfo.targetId));
    });
    if (it == chain.targets.end()) {
      continue;
    }

    // 4. find target info.
    auto targetId = it->targetId;
    auto targetInfo = routingInfo->getTarget(targetId);
    if (UNLIKELY(!targetInfo)) {
      auto msg = fmt::format("targetInfo id {} not found", targetId);
      XLOG(ERR, msg);
      return makeError(StorageClientCode::kRoutingError, std::move(msg));
    }

    // 5. update local target.
    CHECK_RESULT(target, getMutableTarget(targetId));
    bool targetIsServing = targetInfo->publicState == flat::PublicTargetState::SERVING ||
                           targetInfo->publicState == flat::PublicTargetState::SYNCING;
    auto previousLocalState = target->localState;
    target->isHead = (targetIsServing && it == chain.targets.begin());
    target->vChainId = VersionedChainId{chain.chainId, chain.chainVersion};
    if (target->storageTarget != nullptr) {
      if (target->storageTarget->chainId() == ChainId{}) {
        RETURN_AND_LOG_ON_ERROR(target->storageTarget->setChainId(chain.chainId));
      }
      if (target->storageTarget->chainId() != chain.chainId) {
        auto msg = fmt::format("target.chain != routing.chain, target {}, chain {}", *target, chain);
        XLOG(ERR, msg);
        return makeError(StorageClientCode::kRoutingError, std::move(msg));
      }
    }
    target->localState = updateLocalState(targetId, previousLocalState, targetInfo->publicState);
    target->publicState = targetInfo->publicState;
    auto [chainToTargetIt, succ] = chainToTarget_.emplace(chain.chainId, targetId);
    if (!succ) {
      auto msg = fmt::format("chain {} map to 2 targets {}, {}", chain.chainId, chainToTargetIt->second, targetId);
      XLOG(ERR, msg);
      return makeError(StorageClientCode::kRoutingError, std::move(msg));
    }

    if (previousLocalState != flat::LocalTargetState::OFFLINE &&
        target->localState == flat::LocalTargetState::OFFLINE) {
      target->weakStorageTarget = target->storageTarget->aliveWeakPtr();
      target->storageTarget = nullptr;
      continue;
    }

    // 6. update successor.
    while (targetIsServing && ++it != chain.targets.end()) {
      auto targetInfo = routingInfo->getTarget(it->targetId);
      if (UNLIKELY(!targetInfo)) {
        auto msg = fmt::format("successor {} not found", it->targetId);
        XLOG(ERR, msg);
        return makeError(StorageClientCode::kRoutingError, std::move(msg));
      }
      if (targetInfo->publicState == flat::PublicTargetState::SERVING) {
        target->successor = Successor{{}, *targetInfo};
      } else if (targetInfo->publicState == flat::PublicTargetState::SYNCING) {
        target->successor = Successor{{}, *targetInfo};
        syncingChains_.push_back(VersionedChainId{chain.chainId, chain.chainVersion});
      }

      if (target->successor) {
        if (!targetInfo->nodeId.has_value()) {
          XLOGF(WARNING, "target {} node id is nullopt", it->targetId);
          break;
        }
        auto node = routingInfo->getNode(*targetInfo->nodeId);
        if (!node) {
          XLOGF(WARNING, "node {} not found", targetInfo->nodeId);
          break;
        }
        target->successor->nodeInfo = *node;
        if (UNLIKELY(target->successor->nodeInfo.app.serviceGroups.empty())) {
          XLOGF(CRITICAL, "successor invalid! chain {}, successor {}, node {}", chain.chainId, *targetInfo, *node);
          invalidRoutingInfo = true;
        }
      }
      break;
    }
    target->isTail = (targetIsServing && !target->successor.has_value());

    if (headTargets.contains(targetId) ^ target->isHead) {
      if (target->isHead) {
        XLOGF_IF(WARNING, log, "target {} becomes head", targetId);
      } else {
        XLOGF_IF(WARNING, log, "target {} is no longer head", targetId);
      }
    }
    if (tailTargets.contains(targetId) ^ target->isTail) {
      if (target->isTail) {
        XLOGF_IF(WARNING, log, "target {} becomes tail", targetId);
      } else {
        XLOGF_IF(WARNING, log, "target {} is no longer tail", targetId);
      }
    }
  }

  for (auto &[targetId, target] : targets_) {
    if (lastSrvTargets.contains(targetId) && target.storageTarget &&
        (target.publicState == flat::PublicTargetState::SERVING ||
         target.publicState == flat::PublicTargetState::SYNCING ||
         target.publicState == flat::PublicTargetState::WAITING)) {
      target.storageTarget->resetUncommitted(target.vChainId.chainVer);
    }
  }

  recordGuard.succ();
  return Void{};
}

Result<Void> TargetMap::removeTarget(TargetId targetId) {
  auto succ = targets_.erase(targetId);
  if (succ != 1) {
    auto msg = fmt::format("target {} not found", targetId);
    return makeError(StorageClientCode::kRoutingError, std::move(msg));
  }
  return Void{};
}

Result<Void> TargetMap::offlineTarget(TargetId targetId) {
  CHECK_RESULT(target, getMutableTarget(targetId));
  if (target->unrecoverableOffline()) {
    return makeError(StorageCode::kTargetOffline, fmt::format("target is already offline, {}.", *target));
  }

  target->offlineUponUserRequest = true;
  target->localState = flat::LocalTargetState::OFFLINE;
  return Void{};
}

Result<Void> TargetMap::offlineTargets(const Path &path) {
  for (auto &[targetId, target] : targets_) {
    if (path == target.path.parent_path() && !target.unrecoverableOffline()) {
      target.diskError = true;
      target.localState = flat::LocalTargetState::OFFLINE;
      XLOGF(WARNING, "offline target {} because of disk error", target.path);
    }
  }
  return Void{};
}

Result<Void> TargetMap::updateDiskState(const Path &path, bool lowSpace, bool rejectCreateChunk) {
  for (auto &[targetId, target] : targets_) {
    if (path == target.path.parent_path() && !target.unrecoverableOffline()) {
      target.lowSpace = lowSpace;
      auto old = std::exchange(target.rejectCreateChunk, rejectCreateChunk);
      if (old != rejectCreateChunk) {
        XLOGF(WARNING, "target {} reject create chunk {} -> {}", target.path, old, rejectCreateChunk);
      }
    }
  }
  return Void{};
}

hf3fs::flat::LocalTargetState TargetMap::updateLocalState(TargetId targetId,
                                                          hf3fs::flat::LocalTargetState localState,
                                                          hf3fs::flat::PublicTargetState publicState) {
  if (localState == hf3fs::flat::LocalTargetState::UPTODATE &&
      (publicState == hf3fs::flat::PublicTargetState::OFFLINE ||
       publicState == hf3fs::flat::PublicTargetState::LASTSRV ||
       publicState == hf3fs::flat::PublicTargetState::WAITING)) {
    XLOGF(CRITICAL,
          "move to offline state (shutdown), local target: {}, local state: {} -> OFFLINE, public state: {}",
          targetId,
          magic_enum::enum_name(localState),
          magic_enum::enum_name(publicState));
    return hf3fs::flat::LocalTargetState::OFFLINE;
  } else if (localState == hf3fs::flat::LocalTargetState::ONLINE &&
             publicState == hf3fs::flat::PublicTargetState::SERVING) {
    XLOGF(INFO,
          "move to up-to-date state, local target: {}, local state: {} -> UPTODATE, public state: {}",
          targetId,
          magic_enum::enum_name(localState),
          magic_enum::enum_name(publicState));
    return hf3fs::flat::LocalTargetState::UPTODATE;
  }
  return localState;
}

Result<std::shared_ptr<const Target>> AtomicallyTargetMap::getByChainId(
    VersionedChainId vChainId,
    bool allowOutdatedChainVer /* = false */) const {
  auto map = snapshot();
  auto result = map->getByChainId(vChainId, allowOutdatedChainVer);
  RETURN_ON_ERROR(result);
  return std::shared_ptr<const Target>(std::move(map), *result);
}

Result<std::shared_ptr<const Target>> AtomicallyTargetMap::getByTargetId(TargetId targetId) const {
  auto map = snapshot();
  auto result = map->getTarget(targetId);
  RETURN_ON_ERROR(result);
  return std::shared_ptr<const Target>(std::move(map), *result);
}

Result<Void> AtomicallyTargetMap::updateTargetMap(auto &&updateFunc) {
  auto lock = std::unique_lock(mutex_);
  auto map = snapshot();
  while (true) {
    auto newMap = map->clone();
    RETURN_AND_LOG_ON_ERROR(updateFunc(newMap));
    if (targetMap_.compare_exchange_strong(map, std::move(newMap))) {
      break;
    }
  }
  updateCallback_(*snapshot());
  return Void{};
};

Result<Void> AtomicallyTargetMap::addStorageTarget(std::shared_ptr<StorageTarget> storageTarget) {
  return updateTargetMap([&](std::shared_ptr<TargetMap> &newMap) { return newMap->addStorageTarget(storageTarget); });
}

Result<Void> AtomicallyTargetMap::syncReceiveDone(VersionedChainId vChainId) {
  return updateTargetMap([&](std::shared_ptr<TargetMap> &newMap) { return newMap->syncReceiveDone(vChainId); });
}

Result<Void> AtomicallyTargetMap::updateRouting(std::shared_ptr<hf3fs::client::RoutingInfo> r) {
  return updateTargetMap([&](std::shared_ptr<TargetMap> &newMap) { return newMap->updateRouting(r); });
}

Result<Void> AtomicallyTargetMap::removeTarget(TargetId targetId) {
  return updateTargetMap([&](std::shared_ptr<TargetMap> &newMap) { return newMap->removeTarget(targetId); });
}

Result<Void> AtomicallyTargetMap::offlineTarget(TargetId targetId) {
  return updateTargetMap([&](std::shared_ptr<TargetMap> &newMap) { return newMap->offlineTarget(targetId); });
}

Result<Void> AtomicallyTargetMap::offlineTargets(const Path &path) {
  return updateTargetMap([&](std::shared_ptr<TargetMap> &newMap) { return newMap->offlineTargets(path); });
}

Result<Void> AtomicallyTargetMap::updateDiskState(const Path &path, bool lowSpace, bool rejectCreateChunk) {
  return updateTargetMap(
      [&](std::shared_ptr<TargetMap> &newMap) { return newMap->updateDiskState(path, lowSpace, rejectCreateChunk); });
}

void AtomicallyTargetMap::updateTargetUsedSize() {
  auto lock = std::unique_lock(mutex_);
  updateCallback_(*snapshot());
}

}  // namespace hf3fs::storage
