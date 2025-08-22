#pragma once

#include "PersistentNodeInfo.h"
#include "common/utils/UtcTimeSerde.h"

namespace hf3fs::flat {
struct MgmtdLeaseInfo : public serde::SerdeHelper<MgmtdLeaseInfo> {
  MgmtdLeaseInfo() = default;
  MgmtdLeaseInfo(PersistentNodeInfo primaryInfo, UtcTime leaseStart, UtcTime leaseEnd)
      : primary(std::move(primaryInfo)),
        leaseStart(leaseStart),
        leaseEnd(leaseEnd),
        releaseVersion(ReleaseVersion::fromVersionInfo()) {}
  MgmtdLeaseInfo(PersistentNodeInfo primaryInfo, UtcTime leaseStart, UtcTime leaseEnd, ReleaseVersion rv)
      : primary(std::move(primaryInfo)),
        leaseStart(leaseStart),
        leaseEnd(leaseEnd),
        releaseVersion(rv) {}

  SERDE_STRUCT_FIELD(primary, PersistentNodeInfo{});
  SERDE_STRUCT_FIELD(leaseStart, UtcTime{});
  SERDE_STRUCT_FIELD(leaseEnd, UtcTime{});
  SERDE_STRUCT_FIELD(releaseVersion, ReleaseVersion());
};
}  // namespace hf3fs::flat
