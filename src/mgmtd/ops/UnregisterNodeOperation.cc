#include "UnregisterNodeOperation.h"

#include "common/utils/StringUtils.h"
#include "fbs/mgmtd/NodeConversion.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<UnregisterNodeRsp> UnregisterNodeOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  CO_RETURN_ON_ERROR(co_await state.validateAdmin(*this, req.user));

  auto nodeId = req.nodeId;

  auto handler = [&]() -> CoTryTask<UnregisterNodeRsp> {
    auto writerLock = co_await state.coScopedLock<"UnregisterNode">();

    {
      auto dataPtr = co_await state.data_.coSharedLock();
      const auto &ri = dataPtr->routingInfo;
      auto it = ri.nodeMap.find(nodeId);
      if (it == ri.nodeMap.end()) {
        CO_RETURN_AND_LOG_OP_ERR(*this, MgmtdCode::kNodeNotFound, "{} not found", nodeId);
      }
      const auto &node = it->second.base();
      if (node.status != flat::NodeStatus::DISABLED && node.status != flat::NodeStatus::HEARTBEAT_FAILED) {
        CO_RETURN_AND_LOG_OP_ERR(*this,
                                 StatusCode::kInvalidArg,
                                 "Do not allow to unregister {} of status {}",
                                 nodeId,
                                 toStringView(node.status));
      }

      if (node.type == flat::NodeType::STORAGE) {
        for (const auto &[tid, ti] : ri.getTargets()) {
          if (ti.base().nodeId == nodeId) {
            CO_RETURN_AND_LOG_OP_ERR(*this,
                                     StatusCode::kInvalidArg,
                                     "Do not allow to unregister {}: referenced by {} of {}",
                                     nodeId,
                                     tid,
                                     ti.base().chainId);
          }
        }
      }
    }

    auto persistRes =
        co_await updateStoredRoutingInfo(state, *this, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
          co_return co_await state.store_.clearNodeInfo(txn, nodeId);
        });
    CO_RETURN_ON_ERROR(persistRes);

    co_await updateMemoryRoutingInfo(state, *this, [&](RoutingInfo &ri) { ri.nodeMap.erase(nodeId); });

    co_return UnregisterNodeRsp::create();
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}

}  // namespace hf3fs::mgmtd
