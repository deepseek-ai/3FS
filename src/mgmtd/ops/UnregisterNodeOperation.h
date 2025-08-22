#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct UnregisterNodeOperation : core::ServiceOperationWithMetric<"MgmtdService", "UnregisterNode", "op"> {
  UnregisterNodeReq req;

  explicit UnregisterNodeOperation(UnregisterNodeReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("UnregisterNode {}", req.nodeId); }

  CoTryTask<UnregisterNodeRsp> handle(MgmtdState &state);
};

}  // namespace hf3fs::mgmtd
