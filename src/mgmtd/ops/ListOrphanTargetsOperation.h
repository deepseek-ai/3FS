#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct ListOrphanTargetsOperation : core::ServiceOperationWithMetric<"MgmtdService", "ListOrphanTargets", "op"> {
  ListOrphanTargetsReq req;

  explicit ListOrphanTargetsOperation(ListOrphanTargetsReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("ListOrphanTargets"); }

  CoTryTask<ListOrphanTargetsRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
