#pragma once

#include "ICommonMgmtdClient.h"
#include "fbs/mgmtd/ClientSessionData.h"

namespace hf3fs::client {
class IMgmtdClientForClient : public ICommonMgmtdClient {
 public:
  virtual CoTryTask<void> extendClientSession() = 0;

  using ConfigListener = std::function<hf3fs::Result<hf3fs::Void>(const String &, const String &)>;
  virtual void setConfigListener(ConfigListener listener) = 0;

  struct ClientSessionPayload {
    String clientId;
    flat::NodeType nodeType = flat::NodeType::CLIENT;
    flat::ClientSessionData data;
    flat::UserInfo userInfo;
  };

  virtual void setClientSessionPayload(ClientSessionPayload payload) = 0;
};
}  // namespace hf3fs::client
