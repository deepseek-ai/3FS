#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {

struct GetRoutingInfoOperation : core::ServiceOperationWithMetric<"MgmtdService", "GetRoutingInfo", "op"> {
  GetRoutingInfoReq req;

  explicit GetRoutingInfoOperation(GetRoutingInfoReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("GetRoutingInfo for {}", req.routingInfoVersion); }

  CoTryTask<GetRoutingInfoRsp> handle(MgmtdState &state);
};

}  // namespace hf3fs::mgmtd
