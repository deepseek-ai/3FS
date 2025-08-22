#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct SetNodeTagsOperation : core::ServiceOperationWithMetric<"MgmtdService", "SetNodeTags", "op"> {
  SetNodeTagsReq req;

  explicit SetNodeTagsOperation(SetNodeTagsReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return fmt::format("SetNodeTags {}", req.nodeId); }

  CoTryTask<SetNodeTagsRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
