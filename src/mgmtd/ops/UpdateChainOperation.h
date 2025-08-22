#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct UpdateChainOperation : core::ServiceOperationWithMetric<"MgmtdService", "UpdateChain", "op"> {
  UpdateChainReq req;

  explicit UpdateChainOperation(UpdateChainReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("UpdateChain {}", req.chainId); }

  CoTryTask<UpdateChainRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
