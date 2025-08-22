#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct SetChainsOperation : core::ServiceOperationWithMetric<"MgmtdService", "SetChains", "op"> {
  SetChainsReq req;

  explicit SetChainsOperation(SetChainsReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return "SetChains"; }

  CoTryTask<SetChainsRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
