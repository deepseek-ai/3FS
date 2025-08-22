#pragma once

#include "NodeInfo.h"
#include "PersistentNodeInfo.h"

namespace hf3fs::flat {
NodeInfo toNode(const PersistentNodeInfo &pn);
PersistentNodeInfo toPersistentNode(const NodeInfo &node);
}  // namespace hf3fs::flat
