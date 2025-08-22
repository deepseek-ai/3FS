#include "SetNodeTagsOperation.h"

#include "fbs/mgmtd/NodeConversion.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<SetNodeTagsRsp> SetNodeTagsOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  CO_RETURN_ON_ERROR(co_await state.validateAdmin(*this, req.user));

  auto nodeId = req.nodeId;
  for (const auto &tp : req.tags) {
    if (tp.key.empty()) {
      CO_RETURN_AND_LOG_OP_ERR(*this, MgmtdCode::kInvalidTag, "tag key is empty");
    }
  }

  auto handler = [&]() -> CoTryTask<SetNodeTagsRsp> {
    auto writerLock = co_await state.coScopedLock<"SetNodeTags">();

    flat::NodeInfo node;
    {
      auto dataPtr = co_await state.data_.coSharedLock();
      const auto &ri = dataPtr->routingInfo;
      auto it = ri.nodeMap.find(nodeId);
      if (it == ri.nodeMap.end()) CO_RETURN_AND_LOG_OP_ERR(*this, MgmtdCode::kNodeNotFound, "");
      node = it->second.base();
    }

    auto updateRes = updateTags(*this, req.mode, node.tags, req.tags);
    CO_RETURN_ON_ERROR(updateRes);

    XLOGF(DBG,
          "update = {} node.tags = {} newTags = {} oldTags == newTags? {}",
          serde::toJsonString(req.tags),
          serde::toJsonString(node.tags),
          serde::toJsonString(*updateRes),
          node.tags == *updateRes ? "true" : "false");

    if (node.tags != *updateRes) {
      auto oldTags = std::move(node.tags);
      node.tags = std::move(*updateRes);

      if (findTag(node.tags, flat::kDisabledTagKey) == -1 && node.status == flat::NodeStatus::DISABLED) {
        node.status = flat::NodeStatus::HEARTBEAT_CONNECTING;
      }
      if (findTag(node.tags, flat::kDisabledTagKey) != -1 && node.status != flat::NodeStatus::DISABLED) {
        node.status = flat::NodeStatus::DISABLED;
      }

      CO_RETURN_ON_ERROR(
          co_await updateStoredRoutingInfo(state, *this, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
            co_return co_await state.store_.storeNodeInfo(txn, flat::toPersistentNode(node));
          }));

      LOG_OP_INFO(*this, "change tags from {} to {}", serde::toJsonString(oldTags), serde::toJsonString(node.tags));

      co_await updateMemoryRoutingInfo(state, *this, [&](auto &ri) {
        auto &info = ri.nodeMap[nodeId];
        info.base() = node;
        if (nodeId == state.selfId()) {
          state.selfNodeInfo_.tags = node.tags;
          state.selfPersistentNodeInfo_.tags = node.tags;
        }
      });
    }

    co_return SetNodeTagsRsp::create(std::move(node));
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}
}  // namespace hf3fs::mgmtd
