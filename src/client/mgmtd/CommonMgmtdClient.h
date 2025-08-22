#pragma once

#include "ICommonMgmtdClient.h"
#include "MgmtdClient.h"

namespace hf3fs::client {
template <typename Base>
class CommonMgmtdClient : public Base {
 public:
  explicit CommonMgmtdClient(std::shared_ptr<MgmtdClient> client)
      : client_(std::move(client)) {}

  CoTask<void> start(folly::Executor *backgroundExecutor = nullptr, bool startBackground = true) final {
    return client_->start(backgroundExecutor, startBackground);
  }

  CoTask<void> startBackgroundTasks() final { return client_->startBackgroundTasks(); }

  CoTask<void> stop() final { return client_->stop(); }

  std::shared_ptr<RoutingInfo> getRoutingInfo() final { return client_->getRoutingInfo(); }

  CoTryTask<void> refreshRoutingInfo(bool force) final { return client_->refreshRoutingInfo(force); }

  bool addRoutingInfoListener(String name, ICommonMgmtdClient::RoutingInfoListener listener) final {
    return client_->addRoutingInfoListener(std::move(name), std::move(listener));
  }

  bool removeRoutingInfoListener(std::string_view name) final { return client_->removeRoutingInfoListener(name); }

  CoTryTask<mgmtd::ListClientSessionsRsp> listClientSessions() final { return client_->listClientSessions(); }

  CoTryTask<std::optional<flat::ConfigInfo>> getConfig(flat::NodeType nodeType, flat::ConfigVersion version) final {
    return client_->getConfig(nodeType, version);
  }

  CoTryTask<std::vector<flat::TagPair>> getUniversalTags(const String &universalId) final {
    return client_->getUniversalTags(universalId);
  }

  CoTryTask<RHStringHashMap<flat::ConfigVersion>> getConfigVersions() final { return client_->getConfigVersions(); }

  CoTryTask<mgmtd::GetClientSessionRsp> getClientSession(const String &clientId) final {
    return client_->getClientSession(clientId);
  }

 protected:
  std::shared_ptr<MgmtdClient> client_;
};
}  // namespace hf3fs::client
