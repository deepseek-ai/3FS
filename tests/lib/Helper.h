#pragma once

#include <folly/experimental/coro/BlockingWait.h>

#include "common/serde/Serde.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "storage/service/StorageServer.h"
#include "storage/store/StorageTargets.h"

namespace hf3fs::test {

struct StorageServerHelper {
  static auto &components(storage::StorageServer &server) { return server.components_; }
};

struct StorageTargetsHelper {
  static auto &storageTargets(storage::StorageServer &server) {
    return StorageServerHelper::components(server).storageTargets;
  }
};

struct RoutingStoreHelper {
  static void setMgmtdClient(storage::StorageServer &server,
                             std::unique_ptr<hf3fs::client::IMgmtdClientForServer> client) {
    auto &components = StorageServerHelper::components(server);
    auto old = components.mgmtdClient.load();
    components.mgmtdClient.store(std::move(client));
    folly::coro::blockingWait(old->stop());
    components.refreshRoutingInfo();
  }

  static auto getMgmtdClient(storage::StorageServer &server) {
    return StorageServerHelper::components(server).mgmtdClient.load();
  }

  static void refreshRoutingInfo(storage::StorageServer &server) {
    auto &components = StorageServerHelper::components(server);
    folly::coro::blockingWait(components.mgmtdClient.load()->refreshRoutingInfo(true));
    if (dynamic_cast<hf3fs::client::MgmtdClientForServer *>(components.mgmtdClient.load().get()) == nullptr) {
      components.refreshRoutingInfo();
    }
  }
};

struct TargetMapHelper {
  static auto &targetMap(storage::StorageServer &server) { return StorageServerHelper::components(server).targetMap; }

  static auto setLocalTargetState(storage::StorageServer &server, flat::LocalTargetState state) {
    auto &tm = targetMap(server);
    auto map = std::make_shared<storage::TargetMap>(*tm.snapshot());
    for (auto &[targetId, target] : map->targets_) {
      target.localState = state;
    }
    tm.targetMap_.store(std::move(map));
  }

  static auto checkLocalTargetState(storage::StorageServer &server, flat::LocalTargetState state) {
    auto snapshot = targetMap(server).snapshot().get();
    for (auto &[targetId, target] : snapshot->targets_) {
      if (target.localState != state) {
        XLOGF(ERR,
              "check state failed: {} != {}, target {}",
              magic_enum::enum_name(target.localState),
              magic_enum::enum_name(state),
              targetId);
        return false;
      }
    }
    return true;
  }
};

}  // namespace hf3fs::test
