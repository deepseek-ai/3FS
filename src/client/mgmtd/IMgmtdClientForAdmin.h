#pragma once

#include "ICommonMgmtdClient.h"
#include "fbs/mgmtd/ChainSetting.h"
#include "fbs/mgmtd/ConfigInfo.h"

namespace hf3fs::client {
class IMgmtdClientForAdmin : public ICommonMgmtdClient {
 public:
  virtual CoTryTask<mgmtd::SetChainsRsp> setChains(const flat::UserInfo &userInfo,
                                                   const std::vector<flat::ChainSetting> &chains) = 0;

  virtual CoTryTask<mgmtd::SetChainTableRsp> setChainTable(const flat::UserInfo &userInfo,
                                                           flat::ChainTableId tableId,
                                                           const std::vector<flat::ChainId> &chains,
                                                           const String &desc) = 0;

  virtual CoTryTask<flat::ConfigVersion> setConfig(const flat::UserInfo &userInfo,
                                                   flat::NodeType nodeType,
                                                   const String &content,
                                                   const String &desc) = 0;

  virtual CoTryTask<void> enableNode(const flat::UserInfo &userInfo, flat::NodeId nodeId) = 0;

  virtual CoTryTask<void> disableNode(const flat::UserInfo &userInfo, flat::NodeId nodeId) = 0;

  virtual CoTryTask<void> registerNode(const flat::UserInfo &userInfo, flat::NodeId nodeId, flat::NodeType type) = 0;

  virtual CoTryTask<flat::NodeInfo> setNodeTags(const flat::UserInfo &userInfo,
                                                flat::NodeId nodeId,
                                                const std::vector<flat::TagPair> &tags,
                                                flat::SetTagMode mode) = 0;

  virtual CoTryTask<void> unregisterNode(const flat::UserInfo &userInfo, flat::NodeId nodeId) = 0;

  virtual CoTryTask<std::vector<flat::TagPair>> setUniversalTags(const flat::UserInfo &userInfo,
                                                                 const String &universalId,
                                                                 const std::vector<flat::TagPair> &tags,
                                                                 flat::SetTagMode mode) = 0;

  virtual CoTryTask<flat::ChainInfo> rotateLastSrv(const flat::UserInfo &userInfo, flat::ChainId chainId) = 0;

  virtual CoTryTask<flat::ChainInfo> rotateAsPreferredOrder(const flat::UserInfo &userInfo, flat::ChainId chainId) = 0;

  virtual CoTryTask<flat::ChainInfo> setPreferredTargetOrder(
      const flat::UserInfo &userInfo,
      flat::ChainId chainId,
      const std::vector<flat::TargetId> &preferredTargetOrder) = 0;

  virtual CoTryTask<mgmtd::ListOrphanTargetsRsp> listOrphanTargets() = 0;

  virtual CoTryTask<flat::ChainInfo> updateChain(const flat::UserInfo &userInfo,
                                                 flat::ChainId cid,
                                                 flat::TargetId tid,
                                                 mgmtd::UpdateChainReq::Mode mode) = 0;
};
}  // namespace hf3fs::client
