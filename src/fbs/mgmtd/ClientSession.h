#pragma once

#include "ClientSessionData.h"
#include "common/app/ConfigStatus.h"
#include "common/utils/UtcTimeSerde.h"

namespace hf3fs::mgmtd {
struct ExtendClientSessionReq;
}

namespace hf3fs::flat {
struct ClientSession : public serde::SerdeHelper<ClientSession> {
  SERDE_STRUCT_FIELD(clientId, String{});
  SERDE_STRUCT_FIELD(configVersion, ConfigVersion(0));
  SERDE_STRUCT_FIELD(start, UtcTime{});
  SERDE_STRUCT_FIELD(lastExtend, UtcTime{});
  SERDE_STRUCT_FIELD(universalId, String{});
  SERDE_STRUCT_FIELD(description, String{});
  SERDE_STRUCT_FIELD(serviceGroups, std::vector<ServiceGroupInfo>{});
  SERDE_STRUCT_FIELD(releaseVersion, ReleaseVersion{});
  SERDE_STRUCT_FIELD(configStatus, ConfigStatus::NORMAL);
  SERDE_STRUCT_FIELD(type, flat::NodeType::CLIENT);
  SERDE_STRUCT_FIELD(clientStart, UtcTime{});

 public:
  ClientSession() = default;
  explicit ClientSession(const mgmtd::ExtendClientSessionReq &req, UtcTime now = UtcClock::now());
};
}  // namespace hf3fs::flat
