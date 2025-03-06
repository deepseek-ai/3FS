#pragma once

#include "IMgmtdClientForClient.h"
#include "IMgmtdClientForServer.h"
#include "RoutingInfo.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Duration.h"
#include "common/utils/NamedAddress.h"
#include "stubs/common/IStubFactory.h"
#include "stubs/mgmtd/IMgmtdServiceStub.h"

namespace hf3fs::client {
class MgmtdClient {
 public:
  struct Config : public ConfigBase<Config> {
    CONFIG_ITEM(mgmtd_server_addresses, std::vector<net::NamedAddress>{});
    CONFIG_ITEM(work_queue_size, 100, ConfigCheckers::checkPositive);
    CONFIG_ITEM(network_type, std::optional<net::Address::Type>{});

    CONFIG_HOT_UPDATED_ITEM(enable_auto_refresh, true);
    CONFIG_HOT_UPDATED_ITEM(auto_refresh_interval, 10_s);
    CONFIG_HOT_UPDATED_ITEM(enable_auto_heartbeat, true);
    CONFIG_HOT_UPDATED_ITEM(auto_heartbeat_interval, 10_s);
    CONFIG_HOT_UPDATED_ITEM(enable_auto_extend_client_session, true);
    CONFIG_HOT_UPDATED_ITEM(auto_extend_client_session_interval, 10_s);
    CONFIG_HOT_UPDATED_ITEM(accept_incomplete_routing_info_during_mgmtd_bootstrapping, true);
  };

  using MgmtdStub = mgmtd::IMgmtdServiceStub;
  using MgmtdStubFactory = stubs::IStubFactory<MgmtdStub>;
  using RoutingInfoListener = ICommonMgmtdClient::RoutingInfoListener;
  using ConfigListener = IMgmtdClientForServer::ConfigListener;
  using HeartbeatPayload = IMgmtdClientForServer::HeartbeatPayload;
  using ClientSessionPayload = IMgmtdClientForClient::ClientSessionPayload;

  MgmtdClient(String clusterId, std::unique_ptr<MgmtdStubFactory> stubFactory, const Config &config);
  ~MgmtdClient();

  // backgroundExecutor == nullptr means using the current executor
  CoTask<void> start(folly::Executor *backgroundExecutor = nullptr, bool startBackground = true);

  CoTask<void> startBackgroundTasks();

  CoTask<void> stop();

  std::shared_ptr<RoutingInfo> getRoutingInfo();

  CoTryTask<void> refreshRoutingInfo(bool force);

  CoTryTask<mgmtd::HeartbeatRsp> heartbeat();

  Result<Void> triggerHeartbeat();

  void setAppInfoForHeartbeat(flat::AppInfo info);

  bool addRoutingInfoListener(String name, RoutingInfoListener listener);

  bool removeRoutingInfoListener(std::string_view name);

  void setServerConfigListener(ConfigListener listener);

  void setClientConfigListener(ConfigListener listener);

  void updateHeartbeatPayload(HeartbeatPayload payload);

  CoTryTask<mgmtd::SetChainsRsp> setChains(const flat::UserInfo &userInfo,
                                           const std::vector<flat::ChainSetting> &chains);

  CoTryTask<mgmtd::SetChainTableRsp> setChainTable(const flat::UserInfo &userInfo,
                                                   flat::ChainTableId tableId,
                                                   const std::vector<flat::ChainId> &chains,
                                                   const String &desc);

  void setClientSessionPayload(ClientSessionPayload payload);

  CoTryTask<void> extendClientSession();

  CoTryTask<mgmtd::ListClientSessionsRsp> listClientSessions();

  CoTryTask<flat::ConfigVersion> setConfig(const flat::UserInfo &userInfo,
                                           flat::NodeType nodeType,
                                           const String &content,
                                           const String &desc);

  CoTryTask<std::optional<flat::ConfigInfo>> getConfig(flat::NodeType nodeType, flat::ConfigVersion version);

  CoTryTask<RHStringHashMap<flat::ConfigVersion>> getConfigVersions();

  CoTryTask<void> enableNode(const flat::UserInfo &userInfo, flat::NodeId nodeId);

  CoTryTask<void> disableNode(const flat::UserInfo &userInfo, flat::NodeId nodeId);

  CoTryTask<void> registerNode(const flat::UserInfo &userInfo, flat::NodeId nodeId, flat::NodeType type);

  CoTryTask<void> unregisterNode(const flat::UserInfo &userInfo, flat::NodeId nodeId);

  CoTryTask<flat::NodeInfo> setNodeTags(const flat::UserInfo &userInfo,
                                        flat::NodeId nodeId,
                                        const std::vector<flat::TagPair> &tags,
                                        flat::SetTagMode mode);

  CoTryTask<std::vector<flat::TagPair>> setUniversalTags(const flat::UserInfo &userInfo,
                                                         const String &universalId,
                                                         const std::vector<flat::TagPair> &tags,
                                                         flat::SetTagMode mode);

  CoTryTask<std::vector<flat::TagPair>> getUniversalTags(const String &universalId);

  CoTryTask<mgmtd::GetClientSessionRsp> getClientSession(const String &clientId);

  CoTryTask<flat::ChainInfo> rotateLastSrv(const flat::UserInfo &userInfo, flat::ChainId chainId);

  CoTryTask<flat::ChainInfo> rotateAsPreferredOrder(const flat::UserInfo &userInfo, flat::ChainId chainId);

  CoTryTask<flat::ChainInfo> setPreferredTargetOrder(const flat::UserInfo &userInfo,
                                                     flat::ChainId chainId,
                                                     const std::vector<flat::TargetId> &preferredTargetOrder);

  CoTryTask<mgmtd::ListOrphanTargetsRsp> listOrphanTargets();

  CoTryTask<flat::ChainInfo> updateChain(const flat::UserInfo &userInfo,
                                         flat::ChainId cid,
                                         flat::TargetId tid,
                                         mgmtd::UpdateChainReq::Mode mode);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};
}  // namespace hf3fs::client
