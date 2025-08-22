#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct ExtendClientSessionOperation : core::ServiceOperationWithMetric<"MgmtdService", "ExtendClientSession", "op"> {
  ExtendClientSessionReq req;

  explicit ExtendClientSessionOperation(ExtendClientSessionReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final {
    return fmt::format("ExtendClientSession {}@{}", req.clientId, req.data.universalId);
  }

  CoTryTask<ExtendClientSessionRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
