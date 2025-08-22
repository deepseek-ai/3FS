#include "RegisterNodeOperation.h"

#include "fbs/mgmtd/NodeConversion.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<RegisterNodeRsp> RegisterNodeOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  CO_RETURN_ON_ERROR(co_await state.validateAdmin(*this, req.user));

  auto nodeId = req.nodeId;
  auto type = req.type;
  auto handler = [&]() -> CoTryTask<RegisterNodeRsp> {
    if (nodeId == 0) {
      CO_RETURN_AND_LOG_OP_ERR(*this, MgmtdCode::kRegisterFail, "Node id is 0");
    }

    auto writerLock = co_await state.coScopedLock<"RegisterNode">();

    {
      auto dataPtr = co_await state.data_.coSharedLock();
      if (dataPtr->routingInfo.nodeMap.contains(nodeId))
        CO_RETURN_AND_LOG_OP_ERR(*this, MgmtdCode::kRegisterFail, "Node id {} duplicated", nodeId);
    }

    flat::NodeInfo newNode;
    newNode.app.nodeId = nodeId;
    newNode.type = type;

    auto persistRes =
        co_await updateStoredRoutingInfo(state, *this, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
          co_return co_await state.store_.storeNodeInfo(txn, flat::toPersistentNode(newNode));
        });
    CO_RETURN_ON_ERROR(persistRes);

    co_await updateMemoryRoutingInfo(state, *this, [&](RoutingInfo &ri) {
      auto &node = ri.nodeMap[nodeId];
      node.base() = std::move(newNode);
      node.recordStatusChange(newNode.status, UtcClock::now());
    });

    co_return RegisterNodeRsp::create();
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}

}  // namespace hf3fs::mgmtd
