#pragma once

#include <atomic>
#include <common/utils/RobinHood.h>
#include <folly/concurrency/AtomicSharedPtr.h>
#include <memory>

#include "client/mgmtd/RoutingInfo.h"
#include "common/serde/Serde.h"
#include "common/utils/ConstructLog.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "fbs/mgmtd/NodeInfo.h"
#include "fbs/mgmtd/TargetInfo.h"
#include "fbs/storage/Common.h"
#include "storage/store/StorageTarget.h"

namespace hf3fs::test {
struct TargetMapHelper;
}

namespace hf3fs::storage {

class TargetMap {
 public:
  // [observers] clone current map.
  std::shared_ptr<TargetMap> clone() const { return std::make_shared<TargetMap>(*this); }

  // [observers] get target id by chain id.
  Result<TargetId> getTargetId(ChainId chainId) const;

  // [observers] get target by target id.
  Result<const Target *> getTarget(TargetId targetId) const;

  // [observers] get target by versioned chain id.
  Result<const Target *> getByChainId(VersionedChainId vChainId, bool allowOutdatedChainVer) const;

  // [observers]
  auto &getTargets() const { return targets_; }

  // [observers]
  auto &syncingChains() const { return syncingChains_; }

  // [modifiers] add a new target.
  Result<Void> addStorageTarget(const std::shared_ptr<StorageTarget> &storageTarget);

  // [modifiers] get target by target id.
  Result<Target *> getMutableTarget(TargetId targetId);

  // [modifiers] sync receive is started.
  Result<Void> syncReceiveDone(VersionedChainId vChainId);

  // [modifiers] update by routing info.
  Result<Void> updateRouting(std::shared_ptr<hf3fs::client::RoutingInfo> r, bool log = true);

  // [modifiers] set target as offline.
  Result<Void> removeTarget(TargetId targetId);

  // [modifiers] set target as offline.
  Result<Void> offlineTarget(TargetId targetId);

  // [modifiers] set targets in path as offline.
  Result<Void> offlineTargets(const Path &path);

  // [modifiers] reject create chunk for targets in path.
  Result<Void> updateDiskState(const Path &path, bool lowSpace, bool rejectCreateChunk);

  // update local state.
  static hf3fs::flat::LocalTargetState updateLocalState(TargetId targetId,
                                                        hf3fs::flat::LocalTargetState localState,
                                                        hf3fs::flat::PublicTargetState publicState);

 private:
  friend struct test::TargetMapHelper;
  robin_hood::unordered_map<TargetId, Target> targets_;
  flat::RoutingInfoVersion routingInfoVersion_;
  robin_hood::unordered_map<ChainId, TargetId> chainToTarget_;
  std::vector<VersionedChainId> syncingChains_;
};

class AtomicallyTargetMap {
 public:
  // [observers] get a snapshot of target map.
  auto snapshot() const { return targetMap_.load(); }

  // [observers] get target by chain id.
  Result<std::shared_ptr<const Target>> getByChainId(VersionedChainId vChainId,
                                                     bool allowOutdatedChainVer = false) const;

  // [observers] get target by its id.
  Result<std::shared_ptr<const Target>> getByTargetId(TargetId targetId) const;

  // [modifiers] set update callback.
  void setUpdateCallback(auto &&func) {
    auto lock = std::unique_lock(mutex_);
    updateCallback_ = std::forward<decltype(func)>(func);
  }

  // [modifiers] add a target.
  Result<Void> addStorageTarget(std::shared_ptr<StorageTarget> storageTarget);

  // [modifiers] sync receive is done.
  Result<Void> syncReceiveDone(VersionedChainId vChainId);

  // [modifiers] update by routing info.
  Result<Void> updateRouting(std::shared_ptr<hf3fs::client::RoutingInfo> r);

  // [modifiers] set target as offline.
  Result<Void> removeTarget(TargetId targetId);

  // [modifiers] set target as offline.
  Result<Void> offlineTarget(TargetId targetId);

  // [modifiers] set targets in path as offline.
  Result<Void> offlineTargets(const Path &path);

  // [modifiers] reject create chunk for targets in path.
  Result<Void> updateDiskState(const Path &path, bool lowSpace, bool rejectCreateChunk);

  // [modifiers] update target used size.
  void updateTargetUsedSize();

  // [modifiers] release target map.
  auto release() { return targetMap_.exchange(nullptr); }

 protected:
  // [modifiers] update target map atomically.
  Result<Void> updateTargetMap(auto &&updateFunc);

 private:
  friend struct test::TargetMapHelper;
  ConstructLog<"storage::AtomicallyTargetMap"> constructLog_;
  std::mutex mutex_;  // for update operation.
  std::function<void(const TargetMap &)> updateCallback_ = [](auto) {};
  folly::atomic_shared_ptr<const TargetMap> targetMap_{std::make_shared<const TargetMap>()};
};

}  // namespace hf3fs::storage
