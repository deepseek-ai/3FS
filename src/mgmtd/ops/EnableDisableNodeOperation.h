#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct EnableNodeOperation : core::ServiceOperationWithMetric<"MgmtdService", "EnableNode", "op"> {
  EnableNodeReq req;

  explicit EnableNodeOperation(EnableNodeReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("EnableNode {}", req.nodeId); }

  CoTryTask<EnableNodeRsp> handle(MgmtdState &state);
};

struct DisableNodeOperation : core::ServiceOperationWithMetric<"MgmtdService", "DisableNode", "op"> {
  DisableNodeReq req;

  explicit DisableNodeOperation(DisableNodeReq &&r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("disableNode {}", req.nodeId); }

  CoTryTask<DisableNodeRsp> handle(MgmtdState &state);
};

}  // namespace hf3fs::mgmtd
