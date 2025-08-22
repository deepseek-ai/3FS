#pragma once

#include "RoutingInfo.h"
#include "fbs/mgmtd/Rpc.h"

namespace hf3fs::client {
// ICommonMgmtdClient provides the common facilities:
// 1. start and stop
// 2. RoutingInfo
class ICommonMgmtdClient {
 public:
  virtual ~ICommonMgmtdClient() = default;

  virtual CoTask<void> start(folly::Executor *backgroundExecutor = nullptr, bool startBackground = true) { co_return; }

  virtual CoTask<void> startBackgroundTasks() { co_return; }

  virtual CoTask<void> stop() { co_return; }

  virtual std::shared_ptr<RoutingInfo> getRoutingInfo() = 0;

  // manual calls
  virtual CoTryTask<void> refreshRoutingInfo(bool force) = 0;

  using RoutingInfoListener = std::function<void(std::shared_ptr<RoutingInfo>)>;
  virtual bool addRoutingInfoListener(String name, RoutingInfoListener listener) = 0;
  virtual bool removeRoutingInfoListener(std::string_view name) = 0;

  virtual CoTryTask<mgmtd::ListClientSessionsRsp> listClientSessions() = 0;

  virtual CoTryTask<std::optional<flat::ConfigInfo>> getConfig(flat::NodeType nodeType,
                                                               flat::ConfigVersion version) = 0;

  virtual CoTryTask<std::vector<flat::TagPair>> getUniversalTags(const String &universalId) = 0;

  virtual CoTryTask<RHStringHashMap<flat::ConfigVersion>> getConfigVersions() = 0;

  virtual CoTryTask<mgmtd::GetClientSessionRsp> getClientSession(const String &clientId) = 0;
};
}  // namespace hf3fs::client
