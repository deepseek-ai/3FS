#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {

struct GetPrimaryMgmtdOperation : core::ServiceOperationWithMetric<"MgmtdService", "GetPrimaryMgmtd", "op"> {
  GetPrimaryMgmtdReq req;

  explicit GetPrimaryMgmtdOperation(GetPrimaryMgmtdReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return "GetPrimaryMgmtd"; }

  CoTryTask<GetPrimaryMgmtdRsp> handle(MgmtdState &state) const;
};
}  // namespace hf3fs::mgmtd
