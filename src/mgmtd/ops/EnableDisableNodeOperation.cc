#include "EnableDisableNodeOperation.h"

#include "fbs/mgmtd/NodeConversion.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {
flat::NodeInfo onNodeEnabled(const flat::NodeInfo &oldNodeInfo, UtcTime) {
  flat::NodeInfo sn = oldNodeInfo;
  sn.status = flat::NodeStatus::HEARTBEAT_CONNECTING;
  [[maybe_unused]] auto ok = flat::removeTag(sn.tags, flat::kDisabledTagKey);
  assert(ok);
  return sn;
}

flat::NodeInfo onNodeDisabled(const flat::NodeInfo &oldNodeInfo, UtcTime) {
  flat::NodeInfo sn = oldNodeInfo;
  sn.status = flat::NodeStatus::DISABLED;
  assert(flat::findTag(sn.tags, flat::kDisabledTagKey) == -1);
  sn.tags.emplace_back(flat::kDisabledTagKey, "");
  return sn;
}

template <NameWrapper method>
CoTryTask<void> changeNodeStatus(MgmtdState &state, core::ServiceOperation &ctx, flat::NodeId nodeId, bool enable) {
  auto start = state.utcNow();
  auto writerLock = co_await state.coScopedLock<method>();

  auto oldNodeRes = co_await [&]() -> CoTryTask<flat::NodeInfo> {
    auto dataPtr = co_await state.data_.coSharedLock();
    const auto &ri = dataPtr->routingInfo;
    auto it = ri.nodeMap.find(nodeId);
    if (it == ri.nodeMap.end()) CO_RETURN_AND_LOG_OP_ERR(ctx, MgmtdCode::kNodeNotFound, "");
    co_return it->second.base();
  }();
  CO_RETURN_ON_ERROR(oldNodeRes);

  flat::NodeInfo newNode;
  if (enable && oldNodeRes->status == flat::NodeStatus::DISABLED) {
    newNode = onNodeEnabled(*oldNodeRes, start);
  } else if (!enable && oldNodeRes->status != flat::NodeStatus::DISABLED) {
    newNode = onNodeDisabled(*oldNodeRes, start);
  } else {
    co_return Void{};
  }

  CO_RETURN_ON_ERROR(
      co_await updateStoredRoutingInfo(state, ctx, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
        CO_RETURN_ON_ERROR(co_await state.store_.storeNodeInfo(txn, flat::toPersistentNode(newNode)));
        co_return Void{};
      }));

  co_await updateMemoryRoutingInfo(state, ctx, [&](auto &ri) {
    auto &info = ri.nodeMap[nodeId];
    info.base() = newNode;
    info.recordStatusChange(newNode.status, UtcClock::now());
    info.updateTs();
  });
  co_return Void{};
};
}  // namespace

CoTryTask<EnableNodeRsp> EnableNodeOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  CO_RETURN_ON_ERROR(co_await state.validateAdmin(*this, req.user));

  auto nodeId = req.nodeId;
  if (nodeId == state.selfId()) {
    CO_RETURN_AND_LOG_OP_ERR(*this, StatusCode::kInvalidArg, "Can't enable primary mgmtd");
  }
  co_return co_await doAsPrimary(state, [&]() -> CoTryTask<EnableNodeRsp> {
    CO_RETURN_ON_ERROR(co_await changeNodeStatus<"EnableNode">(state, *this, nodeId, true));
    co_return EnableNodeRsp::create();
  });
}

CoTryTask<DisableNodeRsp> DisableNodeOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  CO_RETURN_ON_ERROR(co_await state.validateAdmin(*this, req.user));
  auto nodeId = req.nodeId;
  if (nodeId == state.selfId()) {
    CO_RETURN_AND_LOG_OP_ERR(*this, StatusCode::kInvalidArg, "Can't disable primary mgmtd");
  }
  co_return co_await doAsPrimary(state, [&]() -> CoTryTask<DisableNodeRsp> {
    CO_RETURN_ON_ERROR(co_await changeNodeStatus<"DisableNode">(state, *this, nodeId, false));
    co_return DisableNodeRsp::create();
  });
}

}  // namespace hf3fs::mgmtd
