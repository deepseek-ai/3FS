#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct SetConfigOperation : core::ServiceOperationWithMetric<"MgmtdService", "SetConfig", "op"> {
  SetConfigReq req;

  explicit SetConfigOperation(SetConfigReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("SetConfig {}", magic_enum::enum_name(req.nodeType)); }

  CoTryTask<SetConfigRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
