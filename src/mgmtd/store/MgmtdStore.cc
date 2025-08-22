#include "MgmtdStore.h"

#include <folly/experimental/coro/Collect.h>
#include <folly/lang/Bits.h>

#include "common/kv/KeyPrefix.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/SerDeser.h"
#include "common/utils/StringUtils.h"
#include "common/utils/Transform.h"
#include "fbs/mgmtd/NodeConversion.h"
#include "mgmtd/service/updateChain.h"

namespace hf3fs::mgmtd {
namespace {
std::string_view getMgmtdLeaseKey() {
  static const std::string key = fmt::format("{}MgmtdLease", kv::toStr(kv::KeyPrefix::Single));
  return key;
}

std::string_view getRoutingInfoVersionKey() {
  static const std::string key = fmt::format("{}RoutingInfoVersion", kv::toStr(kv::KeyPrefix::Single));
  return key;
}

String getNodeKey(flat::NodeId id) {
  String buf;
  Serializer s(buf);
  s.put(kv::KeyPrefix::NodeTable);
  s.put(id.toUnderType());
  return buf;
}

String getChainTableKey(flat::ChainTableId id, flat::ChainTableVersion ver) {
  String buf;
  Serializer s(buf);
  s.put(kv::KeyPrefix::ChainTable);
  s.put(id.toUnderType());
  s.put(ver.toUnderType());
  return buf;
}

String getChainInfoKey(flat::ChainId id) {
  String buf;
  Serializer s(buf);
  s.put(kv::KeyPrefix::ChainInfo);
  s.put(id.toUnderType());
  return buf;
}

String getTargetInfoKey(flat::TargetId id) {
  auto reversedId = folly::Endian::big64(id.toUnderType());
  String buf;
  Serializer s(buf);
  s.put(kv::KeyPrefix::TargetInfo);
  s.put(reversedId);
  return buf;
}

String getKeyForUniversalTags(std::string_view id) {
  return fmt::format("{}{}", kv::toStr(kv::KeyPrefix::UniversalTags), id);
}

std::string_view getKeyPrefixForUniversalTags() { return kv::toStr(kv::KeyPrefix::UniversalTags); }

auto getChainTableKeyPrefix() { return kv::toStr(kv::KeyPrefix::ChainTable); }

auto getChainInfoKeyPrefix() { return kv::toStr(kv::KeyPrefix::ChainInfo); }

Result<flat::NodeId> extractNodeIdFromKey(std::string_view key) {
  Deserializer deser(key);
  auto prefixRes = deser.get<kv::KeyPrefix>();
  RETURN_ON_ERROR(prefixRes);
  if (*prefixRes != kv::KeyPrefix::NodeTable) {
    return makeError(StatusCode::kDataCorruption, fmt::format("Invalid prefix of node: key == {}", key));
  }
  auto idRes = deser.get<flat::NodeId::UnderlyingType>();
  RETURN_ON_ERROR(idRes);
  if (!deser.reachEnd()) {
    return makeError(StatusCode::kDataCorruption, fmt::format("Invalid prefix of node: key == {}", key));
  }
  return flat::NodeId{*idRes};
}

String getConfigKey(flat::NodeType nodeType, flat::ConfigVersion version) {
  String buf;
  Serializer s(buf);
  s.put(kv::KeyPrefix::Config);
  s.putShortString(toStringView(nodeType));

  // let the latest version be the first
  auto reversedBigVer = folly::Endian::big64(std::numeric_limits<uint64_t>::max() - version.toUnderType());
  s.put(reversedBigVer);
  return buf;
}

Result<std::tuple<flat::NodeType, flat::ConfigVersion>> decodeConfigKey(std::string_view s) {
  Deserializer des(s);
  auto keyPrefix = co_await des.get<kv::KeyPrefix>();
  if (keyPrefix != kv::KeyPrefix::Config) {
    co_return makeError(StatusCode::kDataCorruption,
                        fmt::format("Decode config key failed: prefix mismatch. key: {}. expected prefix: {}.",
                                    toHexString(s),
                                    kv::toStr(kv::KeyPrefix::Config)));
  }
  auto typeStr = co_await des.getShortString();
  auto type = magic_enum::enum_cast<flat::NodeType>(typeStr);
  if (!type) {
    co_return makeError(
        StatusCode::kDataCorruption,
        fmt::format("Decode config key failed: unknown type. key: {}. type: {}", toHexString(s), typeStr));
  }
  auto reversedBigVer = co_await des.get<uint64_t>();
  auto reversedLittleVer = folly::Endian::big64(reversedBigVer);
  auto ver = std::numeric_limits<uint64_t>::max() - reversedLittleVer;
  co_return std::make_tuple(*type, flat::ConfigVersion(ver));
}

std::string_view getConfigKeyPrefix() { return kv::toStr(kv::KeyPrefix::Config); }

String getConfigKeyPrefix(flat::NodeType nodeType) {
  String buf;
  Serializer s(buf);
  s.put(kv::KeyPrefix::Config);
  s.putShortString(toStringView(nodeType));
  return buf;
}

CoTryTask<std::pair<flat::NodeType, flat::ConfigInfo>> unpackConfigInfo(const kv::IReadOnlyTransaction::KeyValue &kv,
                                                                        std::optional<flat::NodeType> expectedType) {
  const auto &[k, v] = kv.pair();
  auto keyDecodeRes = decodeConfigKey(k);
  CO_RETURN_ON_ERROR(keyDecodeRes);
  auto [t, version] = *keyDecodeRes;
  if (expectedType && t != expectedType) {
    co_return makeError(
        StatusCode::kDataCorruption,
        fmt::format("Expect {} in config key but get {}. key: {}", toStringView(*expectedType), toStringView(t), k));
  }
  auto valueDecodeRes = flat::ConfigInfo::unpackFrom(v);
  CO_RETURN_ON_ERROR(valueDecodeRes);
  if (version != valueDecodeRes->configVersion) {
    co_return makeError(
        StatusCode::kDataCorruption,
        fmt::format("Version mismatch between key and value. version in key: {}. version in value: {}. key: {}",
                    version.toUnderType(),
                    valueDecodeRes->configVersion.toUnderType(),
                    toHexString(k)));
  }
  co_return std::make_pair(t, std::move(*valueDecodeRes));
}
}  // namespace

CoTryTask<flat::MgmtdLeaseInfo> MgmtdStore::extendLease(kv::IReadWriteTransaction &txn,
                                                        const flat::PersistentNodeInfo &nodeInfo,
                                                        std::chrono::microseconds leaseLength,
                                                        UtcTime now,
                                                        flat::ReleaseVersion rv,
                                                        bool checkReleaseVersion) {
  auto fetchResult = co_await loadMgmtdLeaseInfo(txn);
  CO_RETURN_ON_ERROR(fetchResult);
  std::optional<flat::MgmtdLeaseInfo> storedLeaseInfo = std::move(*fetchResult);

  flat::MgmtdLeaseInfo newLeaseInfo(nodeInfo, now, now + leaseLength, rv);

  if (storedLeaseInfo.has_value()) {
    if (checkReleaseVersion && newLeaseInfo.releaseVersion < storedLeaseInfo->releaseVersion) {
      // releaseVersion should not rollback
      co_return *storedLeaseInfo;
    }
    auto leaseEnd = storedLeaseInfo->leaseEnd;
    if (leaseEnd >= now + leaseLength) {
      // lease is long enough, do nothing
      co_return *storedLeaseInfo;
    }
    if (leaseEnd < now) {
      // lease already retired, try to start a new lease
      co_return co_await storeMgmtdLeaseInfo(txn, newLeaseInfo);
    }
    if (storedLeaseInfo->primary.nodeId == nodeInfo.nodeId) {
      newLeaseInfo.leaseStart = storedLeaseInfo->leaseStart;
      // extend lease
      co_return co_await storeMgmtdLeaseInfo(txn, newLeaseInfo);
    }
    co_return *storedLeaseInfo;
  } else {
    co_return co_await storeMgmtdLeaseInfo(txn, newLeaseInfo);
  }
}

CoTryTask<void> MgmtdStore::ensureLeaseValid(kv::IReadOnlyTransaction &txn, flat::NodeId nodeId, UtcTime now) {
  auto fetchResult = co_await loadMgmtdLeaseInfo(txn);
  CO_RETURN_ON_ERROR(fetchResult);
  std::optional<flat::MgmtdLeaseInfo> storedLeaseInfo = std::move(*fetchResult);
  if (storedLeaseInfo.has_value()) {
    if (storedLeaseInfo->primary.nodeId != nodeId) {
      co_return makeError(MgmtdCode::kNotPrimary, fmt::format("{}", storedLeaseInfo->primary.nodeId));
    }
    if (storedLeaseInfo->leaseEnd < now) co_return makeError(MgmtdCode::kNotPrimary);
    co_return Void{};
  } else {
    co_return makeError(MgmtdCode::kNotPrimary);
  }
}

CoTryTask<void> MgmtdStore::storeNodeInfo(kv::IReadWriteTransaction &txn, const flat::PersistentNodeInfo &info) {
  auto nodeKey = getNodeKey(info.nodeId);
  co_return co_await info.store(txn, nodeKey);
}

CoTryTask<void> MgmtdStore::clearNodeInfo(kv::IReadWriteTransaction &txn, flat::NodeId nodeId) {
  auto nodeKey = getNodeKey(nodeId);
  co_return co_await txn.clear(nodeKey);
}

CoTryTask<std::optional<flat::PersistentNodeInfo>> MgmtdStore::loadNodeInfo(kv::IReadOnlyTransaction &txn,
                                                                            flat::NodeId nodeId,
                                                                            bool snapshotLoad) {
  auto nodeKey = getNodeKey(nodeId);
  if (snapshotLoad)
    co_return co_await flat::PersistentNodeInfo::snapshotLoad(txn, nodeKey);
  else
    co_return co_await flat::PersistentNodeInfo::load(txn, nodeKey);
}

CoTryTask<std::optional<flat::MgmtdLeaseInfo>> MgmtdStore::loadMgmtdLeaseInfo(kv::IReadOnlyTransaction &txn) {
  co_return co_await flat::MgmtdLeaseInfo::load(txn, getMgmtdLeaseKey());
}

CoTryTask<flat::MgmtdLeaseInfo> MgmtdStore::storeMgmtdLeaseInfo(kv::IReadWriteTransaction &txn,
                                                                const flat::MgmtdLeaseInfo &leaseInfo) {
  auto res = co_await leaseInfo.store(txn, getMgmtdLeaseKey());
  CO_RETURN_ON_ERROR(res);
  co_return leaseInfo;
}

CoTryTask<flat::RoutingInfoVersion> MgmtdStore::loadRoutingInfoVersion(kv::IReadOnlyTransaction &txn) {
  auto res = co_await txn.get(getRoutingInfoVersionKey());
  CO_RETURN_ON_ERROR(res);
  if (res->has_value()) {
    Deserializer deser(res->value());
    auto deserRes = deser.get<flat::RoutingInfoVersion::UnderlyingType>();
    CO_RETURN_ON_ERROR(deserRes);
    if (!deser.reachEnd()) {
      co_return makeError(StatusCode::kDataCorruption, "Parse RoutingInfoVersion failed");
    }
    co_return flat::RoutingInfoVersion{*deserRes};
  }
  co_return flat::RoutingInfoVersion{0};
}

CoTryTask<void> MgmtdStore::storeRoutingInfoVersion(kv::IReadWriteTransaction &txn, flat::RoutingInfoVersion version) {
  String buf;
  Serializer ser(buf);
  ser.put(version.toUnderType());

  co_return co_await txn.set(getRoutingInfoVersionKey(), buf);
}

CoTryTask<std::vector<flat::PersistentNodeInfo>> MgmtdStore::loadAllNodes(kv::IReadOnlyTransaction &txn) {
  auto prefix = kv::toStr(kv::KeyPrefix::NodeTable);
  auto listRes = co_await kv::TransactionHelper::listByPrefix(txn, prefix, {});
  CO_RETURN_ON_ERROR(listRes);
  std::vector<flat::PersistentNodeInfo> res;
  for (const auto &kv : *listRes) {
    const auto &[key, value] = kv.pair();
    auto keyRes = extractNodeIdFromKey(key);
    CO_RETURN_ON_ERROR(keyRes);
    auto valueRes = flat::PersistentNodeInfo::unpackFrom(value);
    CO_RETURN_ON_ERROR(valueRes);
    if (*keyRes != valueRes->nodeId) {
      co_return makeError(StatusCode::kDataCorruption,
                          fmt::format("NodeId mismatch when load NodeInfo. id in key: {}. id in value: {}.",
                                      *keyRes,
                                      valueRes->nodeId));
    }
    res.push_back(std::move(*valueRes));
  }
  co_return res;
}

CoTryTask<void> MgmtdStore::storeConfig(kv::IReadWriteTransaction &txn,
                                        flat::NodeType nodeType,
                                        const flat::ConfigInfo &info) {
  auto key = getConfigKey(nodeType, info.configVersion);
  co_return co_await info.store(txn, key);
}

CoTryTask<std::vector<std::pair<flat::NodeType, flat::ConfigInfo>>> MgmtdStore::loadAllConfigs(
    kv::IReadOnlyTransaction &txn) {
  auto prefix = getConfigKeyPrefix();
  auto listRes = co_await kv::TransactionHelper::listByPrefix(txn, prefix, {});
  CO_RETURN_ON_ERROR(listRes);
  std::vector<std::pair<flat::NodeType, flat::ConfigInfo>> res;
  for (const auto &kv : *listRes) {
    auto unpackRes = co_await unpackConfigInfo(kv, std::nullopt);
    CO_RETURN_ON_ERROR(unpackRes);
    res.push_back(std::move(*unpackRes));
  }
  co_return res;
}

CoTryTask<std::optional<flat::ConfigInfo>> MgmtdStore::loadLatestConfig(kv::IReadOnlyTransaction &txn,
                                                                        flat::NodeType type) {
  auto prefix = getConfigKeyPrefix(type);
  auto listRes =
      co_await kv::TransactionHelper::listByPrefix(txn,
                                                   prefix,
                                                   kv::TransactionHelper::ListByPrefixOptions().withLimit(1));
  CO_RETURN_ON_ERROR(listRes);
  if (listRes->empty()) {
    co_return std::nullopt;
  }
  if (listRes->size() != 1) {
    co_return makeError(StatusCode::kDataCorruption,
                        fmt::format("List by limit = 1 but get {} items. prefix: {}", listRes->size(), prefix));
  }
  auto unpackRes = co_await unpackConfigInfo(listRes->front(), type);
  CO_RETURN_ON_ERROR(unpackRes);
  co_return std::move(unpackRes->second);
}

CoTryTask<std::optional<flat::ConfigInfo>> MgmtdStore::loadConfig(kv::IReadOnlyTransaction &txn,
                                                                  flat::NodeType type,
                                                                  flat::ConfigVersion version) {
  auto key = getConfigKey(type, version);
  co_return co_await flat::ConfigInfo::load(txn, key);
}

CoTryTask<void> MgmtdStore::storeChainTable(kv::IReadWriteTransaction &txn, const flat::ChainTable &chainTable) {
  auto key = getChainTableKey(chainTable.chainTableId, chainTable.chainTableVersion);
  co_return co_await chainTable.store(txn, key);
}

CoTryTask<std::vector<flat::ChainTable>> MgmtdStore::loadAllChainTables(kv::IReadOnlyTransaction &txn) {
  auto prefix = getChainTableKeyPrefix();
  auto listRes = co_await kv::TransactionHelper::listByPrefix(txn, prefix, {});
  CO_RETURN_ON_ERROR(listRes);
  std::vector<flat::ChainTable> res;
  for (const auto &kv : *listRes) {
    auto unpackRes = flat::ChainTable::unpackFrom(kv.value);
    CO_RETURN_ON_ERROR(unpackRes);
    res.push_back(std::move(*unpackRes));
  }
  co_return res;
}

CoTryTask<void> MgmtdStore::storeChainInfo(kv::IReadWriteTransaction &txn, const flat::ChainInfo &chainInfo) {
  auto key = getChainInfoKey(chainInfo.chainId);
  co_return co_await chainInfo.store(txn, key);
}

CoTryTask<std::vector<flat::ChainInfo>> MgmtdStore::loadAllChains(kv::IReadOnlyTransaction &txn) {
  auto prefix = getChainInfoKeyPrefix();
  auto listRes = co_await kv::TransactionHelper::listByPrefix(txn, prefix, {});
  CO_RETURN_ON_ERROR(listRes);
  std::vector<flat::ChainInfo> res;
  for (const auto &kv : *listRes) {
    auto unpackRes = flat::ChainInfo::unpackFrom(kv.value);
    CO_RETURN_ON_ERROR(unpackRes);
    res.push_back(std::move(*unpackRes));
  }
  co_return res;
}

CoTryTask<void> MgmtdStore::storeRoutingInfo(kv::IReadWriteTransaction &txn, const flat::RoutingInfo &routingInfo) {
  CO_RETURN_ON_ERROR(co_await storeRoutingInfoVersion(txn, routingInfo.routingInfoVersion));
  for ([[maybe_unused]] const auto &[id, node] : routingInfo.nodes) {
    CO_RETURN_ON_ERROR(co_await storeNodeInfo(txn, toPersistentNode(node)));
  }
  for ([[maybe_unused]] const auto &[_, vm] : routingInfo.chainTables) {
    for ([[maybe_unused]] const auto &[_, table] : vm) {
      CO_RETURN_ON_ERROR(co_await storeChainTable(txn, table));
    }
  }
  for ([[maybe_unused]] const auto &[_, chain] : routingInfo.chains) {
    CO_RETURN_ON_ERROR(co_await storeChainInfo(txn, chain));
  }
  co_return Void{};
}

CoTryTask<void> MgmtdStore::storeUniversalTags(kv::IReadWriteTransaction &txn,
                                               std::string_view id,
                                               const std::vector<flat::TagPair> &tags) {
  if (id.empty()) {
    co_return makeError(StatusCode::kInvalidArg, "empty id");
  }
  auto key = getKeyForUniversalTags(id);
  auto value = serde::serialize(tags);
  co_return co_await txn.set(key, value);
}

CoTryTask<std::vector<flat::TagPair>> MgmtdStore::loadUniversalTags(kv::IReadOnlyTransaction &txn,
                                                                    std::string_view id) {
  if (id.empty()) {
    co_return makeError(StatusCode::kInvalidArg, "empty id");
  }
  auto key = getKeyForUniversalTags(id);
  auto loadRes = co_await txn.snapshotGet(key);
  CO_RETURN_ON_ERROR(loadRes);

  std::vector<flat::TagPair> tags;
  if (loadRes->has_value()) {
    CO_RETURN_ON_ERROR(serde::deserialize(tags, loadRes->value()));
  }
  co_return tags;
}

CoTryTask<std::vector<std::pair<String, std::vector<flat::TagPair>>>> MgmtdStore::loadAllUniversalTags(
    kv::IReadOnlyTransaction &txn) {
  auto prefix = getKeyPrefixForUniversalTags();
  auto loadRes = co_await kv::TransactionHelper::listByPrefix(txn, prefix, {});
  CO_RETURN_ON_ERROR(loadRes);

  std::vector<std::pair<String, std::vector<flat::TagPair>>> vec;
  for (const auto &kv : *loadRes) {
    const auto &[k, v] = kv;
    if (k.size() <= prefix.size()) {
      co_return makeError(
          StatusCode::kDataCorruption,
          fmt::format("Key of UniversalTags should be longer than prefix. key: {}. prefix: {}", k, prefix));
    }
    auto id = k.substr(prefix.size());
    std::vector<flat::TagPair> tags;
    CO_RETURN_ON_ERROR(serde::deserialize(tags, v));
    vec.emplace_back(std::move(id), std::move(tags));
  }
  co_return vec;
}

CoTryTask<void> MgmtdStore::clearUniversalTags(kv::IReadWriteTransaction &txn, std::string_view id) {
  if (id.empty()) {
    co_return makeError(StatusCode::kInvalidArg, "empty id");
  }
  auto key = getKeyForUniversalTags(id);
  co_return co_await txn.clear(key);
}

CoTryTask<void> MgmtdStore::shutdownAllChains(kv::IReadWriteTransaction &txn) {
  auto rivRes = co_await loadRoutingInfoVersion(txn);
  CO_RETURN_ON_ERROR(rivRes);

  ++rivRes->toUnderType();
  CO_RETURN_ON_ERROR(co_await storeRoutingInfoVersion(txn, *rivRes));

  auto chainsRes = co_await loadAllChains(txn);
  CO_RETURN_ON_ERROR(chainsRes);

  for (auto &ci : *chainsRes) {
    auto newTargets = shutdownChain(ci.targets);
    if (ci.targets != newTargets) {
      ++ci.chainVersion.toUnderType();
      ci.targets = std::move(newTargets);
      CO_RETURN_ON_ERROR(co_await storeChainInfo(txn, ci));
    }
  }

  co_return Void{};
}

CoTryTask<void> MgmtdStore::storeTargetInfo(kv::IReadWriteTransaction &txn, const flat::TargetInfo &ti) {
  auto key = getTargetInfoKey(ti.targetId);
  co_return co_await ti.store(txn, key);
}

CoTryTask<void> MgmtdStore::clearTargetInfo(kv::IReadWriteTransaction &txn, flat::TargetId tid) {
  auto key = getTargetInfoKey(tid);
  co_return co_await txn.clear(key);
}

CoTryTask<std::optional<flat::TargetInfo>> MgmtdStore::loadTargetInfo(kv::IReadOnlyTransaction &txn,
                                                                      flat::TargetId tid) {
  auto key = getTargetInfoKey(tid);
  co_return co_await flat::TargetInfo::snapshotLoad(txn, key);
}

CoTryTask<std::vector<flat::TargetInfo>> MgmtdStore::loadTargetsFrom(kv::IReadOnlyTransaction &txn,
                                                                     flat::TargetId tid) {
  auto beginKey = getTargetInfoKey(tid);
  auto beginSel = kv::IReadOnlyTransaction::KeySelector(beginKey, true);
  auto endKey = getTargetInfoKey(flat::TargetId(-1));
  auto endSel = kv::IReadOnlyTransaction::KeySelector(endKey, true);

  auto res = co_await txn.snapshotGetRange(beginSel, endSel, 0);
  std::vector<flat::TargetInfo> targets;
  for (const auto &[_, v] : res->kvs) {
    auto unpack = flat::TargetInfo::unpackFrom(v);
    CO_RETURN_ON_ERROR(unpack);
    targets.push_back(std::move(*unpack));
  }
  co_return targets;
}
}  // namespace hf3fs::mgmtd
