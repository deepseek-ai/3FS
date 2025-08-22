#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct RegisterNodeOperation : core::ServiceOperationWithMetric<"MgmtdService", "RegisterNode", "op"> {
  RegisterNodeReq req;

  explicit RegisterNodeOperation(RegisterNodeReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final {
    return fmt::format("RegisterNode {} as {}", req.nodeId, magic_enum::enum_name(req.type));
  }

  CoTryTask<RegisterNodeRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
