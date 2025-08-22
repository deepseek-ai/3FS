#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct HeartbeatOperation : core::ServiceOperationWithMetric<"MgmtdService", "Heartbeat", "op"> {
  HeartbeatReq req;

  explicit HeartbeatOperation(HeartbeatReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final {
    return fmt::format("Heartbeat from {}@{} type {}",
                       req.info.app.nodeId,
                       req.info.app.hostname,
                       magic_enum::enum_name(req.info.type()));
  }

  CoTryTask<HeartbeatRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
