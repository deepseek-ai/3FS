#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct SetUniversalTagsOperation : core::ServiceOperationWithMetric<"MgmtdService", "SetUniversalTags", "op"> {
  SetUniversalTagsReq req;

  explicit SetUniversalTagsOperation(SetUniversalTagsReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("SetUniversalTags {}", req.universalId); }

  CoTryTask<SetUniversalTagsRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
