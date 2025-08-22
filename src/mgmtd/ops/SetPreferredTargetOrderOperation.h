#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct SetPreferredTargetOrderOperation
    : core::ServiceOperationWithMetric<"MgmtdService", "SetPreferredTargetOrder", "op"> {
  SetPreferredTargetOrderReq req;

  explicit SetPreferredTargetOrderOperation(SetPreferredTargetOrderReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("SetPreferredTargetOrder {}", req.chainId); }

  CoTryTask<SetPreferredTargetOrderRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
