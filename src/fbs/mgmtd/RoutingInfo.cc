#include "RoutingInfo.h"

#include <folly/logging/xlog.h>

namespace hf3fs::flat {
const ChainTable *RoutingInfo::getChainTable(ChainTableId tableId, ChainTableVersion tableVersion) const {
  auto tit = chainTables.find(tableId);
  if (tit == chainTables.end()) return nullptr;
  if (tit->second.empty()) return nullptr;
  // tv == 0 means latest version
  auto vit = tableVersion != 0 ? tit->second.find(tableVersion) : (--tit->second.end());
  if (vit == tit->second.end()) return nullptr;
  return &vit->second;
}

ChainTable *RoutingInfo::getChainTable(ChainTableId tableId, ChainTableVersion tableVersion) {
  const auto *self = this;
  auto *res = self->getChainTable(tableId, tableVersion);
  return const_cast<ChainTable *>(res);
}

std::optional<ChainId> RoutingInfo::getChainId(ChainRef ref) const {
  auto [tid, tv, index] = ref.decode();

  if (tid == 0 && tv == 0) {
    return ChainId(index);
  }

  const auto *table = getChainTable(tid, tv);
  if (!table) return std::nullopt;

  if (index == 0) return std::nullopt;
  index = (index - 1) % table->chains.size();
  return table->chains[index];
}

const ChainInfo *RoutingInfo::getChain(ChainId id) const {
  auto it = chains.find(id);
  if (it != chains.end()) return &it->second;
  return nullptr;
}

ChainInfo *RoutingInfo::getChain(ChainId id) {
  const auto *self = this;
  auto *res = self->getChain(id);
  return const_cast<ChainInfo *>(res);
}

const ChainInfo *RoutingInfo::getChain(ChainRef ref) const {
  auto cid = getChainId(ref);
  return cid ? getChain(*cid) : nullptr;
}

ChainInfo *RoutingInfo::getChain(ChainRef ref) {
  const auto *self = this;
  auto *res = self->getChain(ref);
  return const_cast<ChainInfo *>(res);
}

const NodeInfo *RoutingInfo::getNode(NodeId id) const {
  auto it = nodes.find(id);
  if (it != nodes.end()) return &it->second;
  return nullptr;
}

NodeInfo *RoutingInfo::getNode(NodeId id) {
  const auto *self = this;
  auto *res = self->getNode(id);
  return const_cast<NodeInfo *>(res);
}

const TargetInfo *RoutingInfo::getTarget(TargetId id) const {
  auto it = targets.find(id);
  if (it != targets.end()) return &it->second;
  return nullptr;
}

TargetInfo *RoutingInfo::getTarget(TargetId id) {
  const auto *self = this;
  auto *res = self->getTarget(id);
  return const_cast<TargetInfo *>(res);
}

}  // namespace hf3fs::flat
