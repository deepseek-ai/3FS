#pragma once

#include "ChainRef.h"
#include "ChainTable.h"
#include "MgmtdTypes.h"
#include "NodeInfo.h"
#include "TargetInfo.h"
#include "common/utils/RobinHoodUtils.h"

namespace hf3fs::flat {
struct RoutingInfo : public serde::SerdeHelper<RoutingInfo> {
  // return nullptr if not found
  const ChainTable *getChainTable(ChainTableId tableId, ChainTableVersion tableVersion = ChainTableVersion(0)) const;
  ChainTable *getChainTable(ChainTableId tableId, ChainTableVersion tableVersion = ChainTableVersion(0));

  // return std::nullopt if not found
  std::optional<ChainId> getChainId(ChainRef ref) const;

  // return nullptr if not found
  const ChainInfo *getChain(ChainId id) const;
  ChainInfo *getChain(ChainId id);

  // return nullptr if not found
  const ChainInfo *getChain(ChainRef ref) const;
  ChainInfo *getChain(ChainRef ref);

  // return nullptr if not found
  const NodeInfo *getNode(NodeId id) const;
  NodeInfo *getNode(NodeId id);

  // return nullptr if not found
  const TargetInfo *getTarget(TargetId id) const;
  TargetInfo *getTarget(TargetId id);

  using NodeMap = robin_hood::unordered_map<NodeId, NodeInfo>;
  // ordered
  using ChainTableVersionMap = std::map<ChainTableVersion, ChainTable>;
  using ChainTableMap = robin_hood::unordered_map<ChainTableId, ChainTableVersionMap>;
  using ChainMap = robin_hood::unordered_map<ChainId, ChainInfo>;
  using TargetMap = robin_hood::unordered_map<TargetId, TargetInfo>;

  SERDE_STRUCT_FIELD(routingInfoVersion, RoutingInfoVersion(0));
  SERDE_STRUCT_FIELD(bootstrapping, bool(false));
  SERDE_STRUCT_FIELD(nodes, NodeMap{});
  SERDE_STRUCT_FIELD(chainTables, ChainTableMap{});
  SERDE_STRUCT_FIELD(chains, ChainMap{});
  SERDE_STRUCT_FIELD(targets, TargetMap{});
};
}  // namespace hf3fs::flat
