#include "NodeConversion.h"

namespace hf3fs::flat {
NodeInfo toNode(const PersistentNodeInfo &pn) {
  auto status = findTag(pn.tags, kDisabledTagKey) == -1 ? NodeStatus::HEARTBEAT_CONNECTING : NodeStatus::DISABLED;
  return NodeInfo::create(FbsAppInfo::create(pn.nodeId, pn.hostname, 0u, pn.serviceGroups),
                          pn.type,
                          status,
                          UtcTime{},
                          pn.tags);
}

PersistentNodeInfo toPersistentNode(const NodeInfo &node) {
  return PersistentNodeInfo::create(node.app.nodeId, node.type, node.app.serviceGroups, node.tags, node.app.hostname);
}
}  // namespace hf3fs::flat
