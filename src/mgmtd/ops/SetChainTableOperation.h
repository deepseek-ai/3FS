#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct SetChainTableOperation : core::ServiceOperationWithMetric<"MgmtdService", "SetChainTable", "op"> {
  SetChainTableReq req;

  explicit SetChainTableOperation(SetChainTableReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("SetChainTable {}", req.chainTableId); }

  CoTryTask<SetChainTableRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
