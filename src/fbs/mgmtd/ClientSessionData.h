#pragma once

#include "MgmtdTypes.h"
#include "common/app/AppInfo.h"
#include "common/serde/SerdeComparisons.h"
#include "common/serde/SerdeHelper.h"

namespace hf3fs::flat {
struct ClientSessionData : public serde::SerdeHelper<ClientSessionData> {
  bool operator==(const ClientSessionData &other) const { return serde::equals(*this, other); }

  SERDE_STRUCT_FIELD(universalId, String{});
  SERDE_STRUCT_FIELD(description, String{});
  SERDE_STRUCT_FIELD(serviceGroups, std::vector<ServiceGroupInfo>{});
  SERDE_STRUCT_FIELD(releaseVersion, ReleaseVersion{});
};
}  // namespace hf3fs::flat
