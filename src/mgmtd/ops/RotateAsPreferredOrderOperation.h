#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct RotateAsPreferredOrderOperation
    : core::ServiceOperationWithMetric<"MgmtdService", "RotateAsPreferredOrder", "op"> {
  RotateAsPreferredOrderReq req;

  explicit RotateAsPreferredOrderOperation(RotateAsPreferredOrderReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("RotateAsPreferredOrder {}", req.chainId); }

  CoTryTask<RotateAsPreferredOrderRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
