#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct GetConfigVersionsOperation : core::ServiceOperationWithMetric<"MgmtdService", "GetConfigVersions", "op"> {
  GetConfigVersionsReq req;

  explicit GetConfigVersionsOperation(GetConfigVersionsReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return "GetConfigVersions"; }

  CoTryTask<GetConfigVersionsRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
