#pragma once

#include "ChainSetting.h"
#include "ClientSession.h"
#include "ClientSessionData.h"
#include "ConfigInfo.h"
#include "HeartbeatInfo.h"
#include "NodeInfo.h"
#include "PersistentNodeInfo.h"
#include "RoutingInfo.h"
#include "common/app/ConfigStatus.h"
#include "fbs/core/user/User.h"

namespace hf3fs::mgmtd {
DEFINE_SERDE_HELPER_STRUCT(GetPrimaryMgmtdReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(GetPrimaryMgmtdRsp) {
  SERDE_STRUCT_FIELD(primary, std::optional<flat::PersistentNodeInfo>{});
};

DEFINE_SERDE_HELPER_STRUCT(HeartbeatReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(info, flat::HeartbeatInfo{});
  SERDE_STRUCT_FIELD(timestamp, UtcTime{});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(HeartbeatRsp) { SERDE_STRUCT_FIELD(config, std::optional<flat::ConfigInfo>{}); };

DEFINE_SERDE_HELPER_STRUCT(RegisterNodeReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(nodeId, flat::NodeId(0));
  SERDE_STRUCT_FIELD(type, flat::NodeType(flat::NodeType::MIN));
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(RegisterNodeRsp) { SERDE_STRUCT_FIELD(dummy, Void{}); };

DEFINE_SERDE_HELPER_STRUCT(GetRoutingInfoReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(routingInfoVersion, flat::RoutingInfoVersion(0));
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(GetRoutingInfoRsp) { SERDE_STRUCT_FIELD(info, std::optional<flat::RoutingInfo>{}); };

DEFINE_SERDE_HELPER_STRUCT(SetConfigReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(nodeType, flat::NodeType(flat::NodeType::MIN));
  SERDE_STRUCT_FIELD(content, String{});
  SERDE_STRUCT_FIELD(desc, String{});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(SetConfigRsp) { SERDE_STRUCT_FIELD(configVersion, flat::ConfigVersion(0)); };

DEFINE_SERDE_HELPER_STRUCT(GetConfigReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(nodeType, flat::NodeType(flat::NodeType::MIN));
  SERDE_STRUCT_FIELD(configVersion, flat::ConfigVersion(0));
  SERDE_STRUCT_FIELD(exactVersion, bool{false});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(GetConfigRsp) { SERDE_STRUCT_FIELD(info, std::optional<flat::ConfigInfo>{}); };

DEFINE_SERDE_HELPER_STRUCT(SetChainTableReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(chainTableId, flat::ChainTableId(0));
  SERDE_STRUCT_FIELD(chains, std::vector<flat::ChainId>{});
  SERDE_STRUCT_FIELD(desc, String{});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(SetChainTableRsp) { SERDE_STRUCT_FIELD(chainTableVersion, flat::ChainTableVersion(0)); };

DEFINE_SERDE_HELPER_STRUCT(EnableNodeReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(nodeId, flat::NodeId(0));
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(EnableNodeRsp) { SERDE_STRUCT_FIELD(dummy, Void{}); };

DEFINE_SERDE_HELPER_STRUCT(DisableNodeReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(nodeId, flat::NodeId(0));
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(DisableNodeRsp) { SERDE_STRUCT_FIELD(dummy, Void{}); };

DEFINE_SERDE_HELPER_STRUCT(ExtendClientSessionReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(clientId, String{});
  SERDE_STRUCT_FIELD(clientSessionVersion, flat::ClientSessionVersion(1));
  SERDE_STRUCT_FIELD(configVersion, flat::ConfigVersion(0));
  SERDE_STRUCT_FIELD(data, flat::ClientSessionData{});
  SERDE_STRUCT_FIELD(configStatus, ConfigStatus::NORMAL);
  SERDE_STRUCT_FIELD(type, flat::NodeType::CLIENT);
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
  SERDE_STRUCT_FIELD(clientStart, UtcTime{});
};

DEFINE_SERDE_HELPER_STRUCT(ExtendClientSessionRsp) {
  SERDE_STRUCT_FIELD(config, std::optional<flat::ConfigInfo>{});
  SERDE_STRUCT_FIELD(tags, std::vector<flat::TagPair>{});
};

DEFINE_SERDE_HELPER_STRUCT(ListClientSessionsReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(ListClientSessionsRsp) {
  SERDE_STRUCT_FIELD(bootstrapping, bool(false));
  SERDE_STRUCT_FIELD(sessions, std::vector<flat::ClientSession>{});
  SERDE_STRUCT_FIELD(referencedTags, RHStringHashMap<std::vector<flat::TagPair>>{});
};

DEFINE_SERDE_HELPER_STRUCT(SetNodeTagsReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(nodeId, flat::NodeId(0));
  SERDE_STRUCT_FIELD(tags, std::vector<flat::TagPair>{});
  SERDE_STRUCT_FIELD(mode, flat::SetTagMode(flat::SetTagMode::REPLACE));
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(SetNodeTagsRsp) { SERDE_STRUCT_FIELD(info, flat::NodeInfo{}); };

DEFINE_SERDE_HELPER_STRUCT(UnregisterNodeReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(nodeId, flat::NodeId(0));
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(UnregisterNodeRsp) { SERDE_STRUCT_FIELD(dummy, Void{}); };

DEFINE_SERDE_HELPER_STRUCT(SetChainsReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(chains, std::vector<flat::ChainSetting>{});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(SetChainsRsp) { SERDE_STRUCT_FIELD(dummy, Void{}); };

DEFINE_SERDE_HELPER_STRUCT(SetUniversalTagsReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(universalId, String{});
  SERDE_STRUCT_FIELD(tags, std::vector<flat::TagPair>{});
  SERDE_STRUCT_FIELD(mode, flat::SetTagMode(flat::SetTagMode::REPLACE));
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(SetUniversalTagsRsp) { SERDE_STRUCT_FIELD(tags, std::vector<flat::TagPair>{}); };

DEFINE_SERDE_HELPER_STRUCT(GetUniversalTagsReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(universalId, String{});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(GetUniversalTagsRsp) { SERDE_STRUCT_FIELD(tags, std::vector<flat::TagPair>{}); };

DEFINE_SERDE_HELPER_STRUCT(GetConfigVersionsReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(GetConfigVersionsRsp) {
  SERDE_STRUCT_FIELD(versions, RHStringHashMap<flat::ConfigVersion>{});
};

DEFINE_SERDE_HELPER_STRUCT(GetClientSessionReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(clientId, String{});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(GetClientSessionRsp) {
  SERDE_STRUCT_FIELD(bootstrapping, bool(false));
  SERDE_STRUCT_FIELD(session, std::optional<flat::ClientSession>{});
  SERDE_STRUCT_FIELD(referencedTags, std::vector<flat::TagPair>{});
};

DEFINE_SERDE_HELPER_STRUCT(RotateLastSrvReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(chainId, flat::ChainId(0));
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(RotateLastSrvRsp) { SERDE_STRUCT_FIELD(chain, flat::ChainInfo{}); };

DEFINE_SERDE_HELPER_STRUCT(ListOrphanTargetsReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(ListOrphanTargetsRsp) { SERDE_STRUCT_FIELD(targets, std::vector<flat::TargetInfo>()); };

DEFINE_SERDE_HELPER_STRUCT(SetPreferredTargetOrderReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(chainId, flat::ChainId{0});
  SERDE_STRUCT_FIELD(preferredTargetOrder, std::vector<flat::TargetId>{});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(SetPreferredTargetOrderRsp) { SERDE_STRUCT_FIELD(chain, flat::ChainInfo{}); };

DEFINE_SERDE_HELPER_STRUCT(RotateAsPreferredOrderReq) {
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(chainId, flat::ChainId(0));
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
};

DEFINE_SERDE_HELPER_STRUCT(RotateAsPreferredOrderRsp) { SERDE_STRUCT_FIELD(chain, flat::ChainInfo{}); };

DEFINE_SERDE_HELPER_STRUCT(UpdateChainReq) {
  enum class Mode : uint8_t {
    ADD = 0,
    REMOVE = 1,
  };
  SERDE_STRUCT_FIELD(clusterId, String{});
  SERDE_STRUCT_FIELD(user, flat::UserInfo{});
  SERDE_STRUCT_FIELD(chainId, flat::ChainId(0));
  SERDE_STRUCT_FIELD(targetId, flat::TargetId(0));
  SERDE_STRUCT_FIELD(mode, Mode::ADD);
};

DEFINE_SERDE_HELPER_STRUCT(UpdateChainRsp) { SERDE_STRUCT_FIELD(chain, flat::ChainInfo{}); };

}  // namespace hf3fs::mgmtd
