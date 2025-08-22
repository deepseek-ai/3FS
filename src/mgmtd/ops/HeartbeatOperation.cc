#include "HeartbeatOperation.h"

#include "common/utils/StringUtils.h"
#include "fbs/mgmtd/NodeConversion.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {
flat::NodeInfo onNewNode(const flat::HeartbeatInfo &hb, UtcTime now) {
  flat::NodeInfo sn;
  sn.app = hb.app;
  sn.type = hb.type();
  sn.status = flat::NodeStatus::HEARTBEAT_CONNECTED;
  sn.lastHeartbeatTs = now;
  // sn.tags is empty
  sn.configVersion = hb.configVersion;
  return sn;
}

flat::NodeInfo onNodeChanged(const flat::NodeInfo &oldNodeInfo, const flat::HeartbeatInfo &hb, UtcTime) {
  flat::NodeInfo sn = oldNodeInfo;
  sn.app = hb.app;
  sn.status = flat::NodeStatus::HEARTBEAT_CONNECTED;
  sn.configVersion = hb.configVersion;
  sn.configStatus = hb.configStatus;
  return sn;
}

Result<flat::NodeInfo> prepareNewNodeInfo(MgmtdState &state,
                                          core::ServiceOperation &ctx,
                                          UtcTime now,
                                          const NodeInfoWrapper *oldNode,
                                          const flat::HeartbeatInfo &hb) {
  auto nodeId = hb.app.nodeId;
  if (!oldNode) {
    if (state.config_.allow_heartbeat_from_unregistered()) {
      LOG_OP_INFO(ctx, "auto register");
      return onNewNode(hb, now);
    } else {
      RETURN_AND_LOG_OP_ERR(ctx, MgmtdCode::kHeartbeatFail, "{} not registered", nodeId);
    }
  }

  const auto &oldNodeBase = oldNode->base();
  if (oldNodeBase.type != hb.type()) {
    RETURN_AND_LOG_OP_ERR(ctx,
                          MgmtdCode::kHeartbeatFail,
                          "{} type mismatch: registered {} got {}",
                          nodeId,
                          magic_enum::enum_name(oldNodeBase.type),
                          magic_enum::enum_name(hb.type()));
  }
  if (oldNodeBase.status == flat::NodeStatus::DISABLED) {
    RETURN_AND_LOG_OP_ERR(ctx, MgmtdCode::kHeartbeatFail, "{} is marked as DISABLED", nodeId);
  }

  if (oldNode->lastHbVersion() >= hb.hbVersion) {
    LOG_OP_WARN(ctx, "{} heartbeat version stale: last {} received {}", nodeId, oldNode->lastHbVersion(), hb.hbVersion);
    return makeError(MgmtdCode::kHeartbeatVersionStale, fmt::format("{}", oldNode->lastHbVersion().toUnderType()));
  }

  return onNodeChanged(oldNodeBase, hb, now);
}

Result<Void> prepareHandleHeartbeat(MgmtdState &state,
                                    core::ServiceOperation &ctx,
                                    const MgmtdData &data,
                                    UtcTime now,
                                    const flat::HeartbeatInfo &hb,
                                    const NodeInfoWrapper **oldNodePtr,
                                    flat::NodeInfo &newNode) {
  auto nodeId = hb.app.nodeId;
  RETURN_ON_ERROR(data.checkConfigVersion(ctx, hb.type(), hb.configVersion));

  const auto &ri = data.routingInfo;
  if (ri.nodeMap.contains(nodeId)) {
    *oldNodePtr = &ri.nodeMap.at(nodeId);
  } else {
    *oldNodePtr = nullptr;
  }

  auto newNodeRes = prepareNewNodeInfo(state, ctx, now, *oldNodePtr, hb);
  RETURN_ON_ERROR(newNodeRes);
  newNode = *newNodeRes;

  if (hb.type() == flat::NodeType::STORAGE) {
    robin_hood::unordered_set<flat::TargetId> seenTargets;
    robin_hood::unordered_map<flat::ChainId, std::vector<LocalTargetInfoWithNodeId>> changedChainMap;
    const auto &shb = hb.asStorage();
    for (const auto &lti : shb.targets) {
      if (!seenTargets.insert(lti.targetId).second) {
        RETURN_AND_LOG_OP_ERR(ctx,
                              MgmtdCode::kHeartbeatFail,
                              "Duplicated target id in HeartbeatInfo: {}",
                              lti.targetId);
      }
      using LS = flat::LocalTargetState;
      switch (lti.localState) {
        case LS::INVALID:
          RETURN_AND_LOG_OP_ERR(ctx,
                                MgmtdCode::kHeartbeatFail,
                                "Invalid LocalTargetState {} for {}",
                                toString(lti.localState),
                                lti.targetId);
        case LS::OFFLINE:
        case LS::ONLINE:
        case LS::UPTODATE:
          // valid LocalTargetState
          break;
      }
    }
  }

  return Void{};
}
}  // namespace

CoTryTask<HeartbeatRsp> HeartbeatOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));

  const auto &hb = req.info;
  auto timestamp = req.timestamp;
  auto nodeId = hb.app.nodeId;

  auto handler = [&]() -> CoTryTask<HeartbeatRsp> {
    if (nodeId == state.selfId()) {
      CO_RETURN_AND_LOG_OP_ERR(*this, MgmtdCode::kHeartbeatFail, "Node id {} duplicated", nodeId);
    }
    if (nodeId == 0) {
      CO_RETURN_AND_LOG_OP_ERR(*this, MgmtdCode::kHeartbeatFail, "Node id is 0");
    }

    auto now = state.utcNow();
    const auto validWindow = state.config_.heartbeat_timestamp_valid_window().asUs();
    if (validWindow.count() != 0 && (timestamp + validWindow < now || now + validWindow < timestamp)) {
      CO_RETURN_AND_LOG_OP_ERR(*this,
                               MgmtdCode::kHeartbeatFail,
                               "Too much timestamp deviation. now {} timestamp {}",
                               now.toMicroseconds(),
                               timestamp.toMicroseconds());
    }

    auto writerLock = co_await state.coScopedLock<"Heartbeat">();

    const NodeInfoWrapper *oldNode = nullptr;
    flat::NodeInfo newNode;
    {
      auto dataPtr = co_await state.data_.coSharedLock();
      CO_RETURN_ON_ERROR(prepareHandleHeartbeat(state, *this, *dataPtr, now, hb, &oldNode, newNode));
    }

    auto persistentNewNode = flat::toPersistentNode(newNode);
    auto needPersist = oldNode == nullptr || flat::toPersistentNode(oldNode->base()) != persistentNewNode;
    auto statusChanged = oldNode == nullptr || oldNode->base().status != newNode.status;

    if (needPersist) {
      CO_RETURN_ON_ERROR(
          co_await updateStoredRoutingInfo(state, *this, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
            co_return co_await state.store_.storeNodeInfo(txn, persistentNewNode);
          }));
    }

    if (statusChanged) {
      LOG_OP_INFO(*this,
                  "ReceiveHeartbeat {} node {} status changed to {}",
                  magic_enum::enum_name(newNode.type),
                  newNode.app.nodeId,
                  magic_enum::enum_name(newNode.status));
    }

    {
      auto dataPtr = co_await state.data_.coLock();
      auto steadyNow = SteadyClock::now();
      auto &ri = dataPtr->routingInfo;
      ri.routingInfoChanged |= statusChanged;

      auto &info = ri.nodeMap[nodeId];
      info.base() = std::move(newNode);
      info.base().lastHeartbeatTs = now;
      info.updateTs(steadyNow);
      info.updateHeartbeatVersion(hb.hbVersion);

      if (hb.type() == flat::NodeType::STORAGE) {
        dataPtr->routingInfo.localUpdateTargets(nodeId, hb.asStorage().targets, state.config_);
      }

      if (needPersist) {
        updateMemoryRoutingInfo(ri, *this);
      }

      if (statusChanged) {
        info.recordStatusChange(newNode.status, now);
      }

      HeartbeatRsp rsp;
      rsp.config = dataPtr->getConfig(hb.type(), hb.configVersion, /*latest=*/true);
      co_return rsp;
    }
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}
}  // namespace hf3fs::mgmtd
