#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct ListClientSessionsOperation : core::ServiceOperationWithMetric<"MgmtdService", "ListClientSessions", "op"> {
  ListClientSessionsReq req;

  explicit ListClientSessionsOperation(ListClientSessionsReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("ListClientSessions"); }

  CoTryTask<ListClientSessionsRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
