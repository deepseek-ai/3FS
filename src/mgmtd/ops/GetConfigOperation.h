#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
struct GetConfigOperation : core::ServiceOperationWithMetric<"MgmtdService", "GetConfig", "op"> {
  GetConfigReq req;

  explicit GetConfigOperation(GetConfigReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final {
    return fmt::format("GetConfig {}@{}", magic_enum::enum_name(req.nodeType), req.configVersion);
  }

  CoTryTask<GetConfigRsp> handle(MgmtdState &state);
};
}  // namespace hf3fs::mgmtd
