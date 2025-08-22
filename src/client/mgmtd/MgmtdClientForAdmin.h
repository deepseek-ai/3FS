#pragma once

#include "CommonMgmtdClient.h"
#include "IMgmtdClientForAdmin.h"

namespace hf3fs::client {
class MgmtdClientForAdmin : public CommonMgmtdClient<IMgmtdClientForAdmin> {
 public:
  struct Config : MgmtdClient::Config {
    Config() {
      set_enable_auto_refresh(false);
      set_enable_auto_heartbeat(false);
      set_enable_auto_extend_client_session(false);
      set_accept_incomplete_routing_info_during_mgmtd_bootstrapping(true);
    }
  };

  explicit MgmtdClientForAdmin(std::shared_ptr<MgmtdClient> client)
      : CommonMgmtdClient(std::move(client)) {}

  MgmtdClientForAdmin(String clusterId,
                      std::unique_ptr<MgmtdClient::MgmtdStubFactory> stubFactory,
                      const Config &config)
      : CommonMgmtdClient(std::make_shared<MgmtdClient>(std::move(clusterId), std::move(stubFactory), config)) {}

  CoTryTask<mgmtd::SetChainsRsp> setChains(const flat::UserInfo &userInfo,
                                           const std::vector<flat::ChainSetting> &chains) final {
    return client_->setChains(userInfo, chains);
  }

  CoTryTask<mgmtd::SetChainTableRsp> setChainTable(const flat::UserInfo &userInfo,
                                                   flat::ChainTableId tableId,
                                                   const std::vector<flat::ChainId> &chains,
                                                   const String &desc) final {
    return client_->setChainTable(userInfo, tableId, chains, desc);
  }

  CoTryTask<flat::ConfigVersion> setConfig(const flat::UserInfo &userInfo,
                                           flat::NodeType nodeType,
                                           const String &content,
                                           const String &desc) final {
    return client_->setConfig(userInfo, nodeType, content, desc);
  }

  CoTryTask<void> enableNode(const flat::UserInfo &userInfo, flat::NodeId nodeId) final {
    return client_->enableNode(userInfo, nodeId);
  }

  CoTryTask<void> disableNode(const flat::UserInfo &userInfo, flat::NodeId nodeId) final {
    return client_->disableNode(userInfo, nodeId);
  }

  CoTryTask<void> registerNode(const flat::UserInfo &userInfo, flat::NodeId nodeId, flat::NodeType type) final {
    return client_->registerNode(userInfo, nodeId, type);
  }

  CoTryTask<flat::NodeInfo> setNodeTags(const flat::UserInfo &userInfo,
                                        flat::NodeId nodeId,
                                        const std::vector<flat::TagPair> &tags,
                                        flat::SetTagMode mode) final {
    return client_->setNodeTags(userInfo, nodeId, tags, mode);
  }

  CoTryTask<void> unregisterNode(const flat::UserInfo &userInfo, flat::NodeId nodeId) final {
    return client_->unregisterNode(userInfo, nodeId);
  }

  CoTryTask<std::vector<flat::TagPair>> setUniversalTags(const flat::UserInfo &userInfo,
                                                         const String &universalId,
                                                         const std::vector<flat::TagPair> &tags,
                                                         flat::SetTagMode mode) final {
    return client_->setUniversalTags(userInfo, universalId, tags, mode);
  }

  CoTryTask<flat::ChainInfo> rotateLastSrv(const flat::UserInfo &userInfo, flat::ChainId chainId) final {
    return client_->rotateLastSrv(userInfo, chainId);
  }

  CoTryTask<flat::ChainInfo> rotateAsPreferredOrder(const flat::UserInfo &userInfo, flat::ChainId chainId) final {
    return client_->rotateAsPreferredOrder(userInfo, chainId);
  }

  CoTryTask<flat::ChainInfo> setPreferredTargetOrder(const flat::UserInfo &userInfo,
                                                     flat::ChainId chainId,
                                                     const std::vector<flat::TargetId> &preferredTargetOrder) final {
    return client_->setPreferredTargetOrder(userInfo, chainId, preferredTargetOrder);
  }

  CoTryTask<flat::ChainInfo> updateChain(const flat::UserInfo &userInfo,
                                         flat::ChainId cid,
                                         flat::TargetId tid,
                                         mgmtd::UpdateChainReq::Mode mode) final {
    return client_->updateChain(userInfo, cid, tid, mode);
  }

  CoTryTask<mgmtd::ListOrphanTargetsRsp> listOrphanTargets() final { return client_->listOrphanTargets(); }
};
}  // namespace hf3fs::client
