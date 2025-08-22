#pragma once

#include "ICommonMgmtdClient.h"
#include "fbs/mgmtd/Rpc.h"

namespace hf3fs::client {
class IMgmtdClientForServer : public ICommonMgmtdClient {
 public:
  // manual calls
  virtual CoTryTask<mgmtd::HeartbeatRsp> heartbeat() = 0;

  virtual Result<Void> triggerHeartbeat() = 0;

  // must set before any manual or auto heartbeat
  virtual void setAppInfoForHeartbeat(flat::AppInfo info) = 0;

  // return whether this configuration change is succeeded
  using ConfigListener = std::function<hf3fs::Result<hf3fs::Void>(const String &, const String &)>;
  virtual void setConfigListener(ConfigListener listener) = 0;

  using HeartbeatPayload = flat::HeartbeatInfo::Payload;
  virtual void updateHeartbeatPayload(HeartbeatPayload payload) = 0;
};
}  // namespace hf3fs::client
