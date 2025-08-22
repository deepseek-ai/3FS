#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct RotateLastSrvOperation : core::ServiceOperationWithMetric<"MgmtdService", "RotateLastSrv", "op"> {
  RotateLastSrvReq req;

  explicit RotateLastSrvOperation(RotateLastSrvReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("RotateLastSrv {}", req.chainId); }

  CoTryTask<RotateLastSrvRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
