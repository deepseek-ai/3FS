#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct GetClientSessionOperation : core::ServiceOperationWithMetric<"MgmtdService", "GetClientSession", "op"> {
  GetClientSessionReq req;

  explicit GetClientSessionOperation(GetClientSessionReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("GetClientSession {}", req.clientId); }

  CoTryTask<GetClientSessionRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
