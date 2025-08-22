#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/UtcTime.h"
#include "fbs/mgmtd/ChainInfo.h"
#include "fbs/mgmtd/ChainTable.h"
#include "fbs/mgmtd/ConfigInfo.h"
#include "fbs/mgmtd/MgmtdLeaseInfo.h"
#include "fbs/mgmtd/PersistentNodeInfo.h"
#include "fbs/mgmtd/RoutingInfo.h"

namespace hf3fs::mgmtd {
class MgmtdStore {
 public:
  MgmtdStore() = default;

  // return current lease
  CoTryTask<flat::MgmtdLeaseInfo> extendLease(kv::IReadWriteTransaction &txn,
                                              const flat::PersistentNodeInfo &nodeInfo,
                                              std::chrono::microseconds leaseLength,
                                              UtcTime now,
                                              flat::ReleaseVersion rv = flat::ReleaseVersion::fromVersionInfo(),
                                              bool checkReleaseVersion = true);
  CoTryTask<void> ensureLeaseValid(kv::IReadOnlyTransaction &txn, flat::NodeId nodeId, UtcTime now);

  CoTryTask<void> storeNodeInfo(kv::IReadWriteTransaction &txn, const flat::PersistentNodeInfo &info);
  CoTryTask<void> clearNodeInfo(kv::IReadWriteTransaction &txn, flat::NodeId nodeId);
  CoTryTask<std::optional<flat::PersistentNodeInfo>> loadNodeInfo(kv::IReadOnlyTransaction &txn,
                                                                  flat::NodeId nodeId,
                                                                  bool snapshotLoad = false);

  CoTryTask<flat::RoutingInfoVersion> loadRoutingInfoVersion(kv::IReadOnlyTransaction &txn);
  CoTryTask<void> storeRoutingInfoVersion(kv::IReadWriteTransaction &txn, flat::RoutingInfoVersion version);

  CoTryTask<std::vector<flat::PersistentNodeInfo>> loadAllNodes(kv::IReadOnlyTransaction &txn);

  CoTryTask<std::optional<flat::MgmtdLeaseInfo>> loadMgmtdLeaseInfo(kv::IReadOnlyTransaction &txn);

  CoTryTask<flat::MgmtdLeaseInfo> storeMgmtdLeaseInfo(kv::IReadWriteTransaction &txn,
                                                      const flat::MgmtdLeaseInfo &leaseInfo);

  CoTryTask<void> storeConfig(kv::IReadWriteTransaction &txn, flat::NodeType nodeType, const flat::ConfigInfo &info);

  CoTryTask<std::vector<std::pair<flat::NodeType, flat::ConfigInfo>>> loadAllConfigs(kv::IReadOnlyTransaction &txn);

  CoTryTask<std::optional<flat::ConfigInfo>> loadLatestConfig(kv::IReadOnlyTransaction &txn, flat::NodeType type);

  CoTryTask<std::optional<flat::ConfigInfo>> loadConfig(kv::IReadOnlyTransaction &txn,
                                                        flat::NodeType type,
                                                        flat::ConfigVersion version);

  CoTryTask<void> storeChainTable(kv::IReadWriteTransaction &txn, const flat::ChainTable &chainTable);

  CoTryTask<std::vector<flat::ChainTable>> loadAllChainTables(kv::IReadOnlyTransaction &txn);

  CoTryTask<void> storeChainInfo(kv::IReadWriteTransaction &txn, const flat::ChainInfo &chainInfo);

  CoTryTask<std::vector<flat::ChainInfo>> loadAllChains(kv::IReadOnlyTransaction &txn);

  // test only
  CoTryTask<void> storeRoutingInfo(kv::IReadWriteTransaction &txn, const flat::RoutingInfo &routingInfo);

  CoTryTask<void> storeUniversalTags(kv::IReadWriteTransaction &txn,
                                     std::string_view id,
                                     const std::vector<flat::TagPair> &tags);

  CoTryTask<std::vector<flat::TagPair>> loadUniversalTags(kv::IReadOnlyTransaction &txn, std::string_view id);

  CoTryTask<std::vector<std::pair<String, std::vector<flat::TagPair>>>> loadAllUniversalTags(
      kv::IReadOnlyTransaction &txn);

  CoTryTask<void> clearUniversalTags(kv::IReadWriteTransaction &txn, std::string_view id);

  CoTryTask<void> shutdownAllChains(kv::IReadWriteTransaction &txn);

  CoTryTask<std::optional<flat::TargetInfo>> loadTargetInfo(kv::IReadOnlyTransaction &txn, flat::TargetId tid);

  CoTryTask<void> storeTargetInfo(kv::IReadWriteTransaction &txn, const flat::TargetInfo &ti);

  CoTryTask<void> clearTargetInfo(kv::IReadWriteTransaction &txn, flat::TargetId tid);

  CoTryTask<std::vector<flat::TargetInfo>> loadTargetsFrom(kv::IReadOnlyTransaction &txn, flat::TargetId tid);
};
}  // namespace hf3fs::mgmtd
