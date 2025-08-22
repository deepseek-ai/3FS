#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct GetUniversalTagsOperation : core::ServiceOperationWithMetric<"MgmtdService", "GetUniversalTags", "op"> {
  GetUniversalTagsReq req;

  explicit GetUniversalTagsOperation(GetUniversalTagsReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("GetUniversalTags {}", req.universalId); }

  CoTryTask<GetUniversalTagsRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
