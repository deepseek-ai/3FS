#pragma once

#include "common/app/ApplicationBase.h"
#include "core/utils/ServiceOperation.h"
#include "fbs/core/service/Rpc.h"

namespace hf3fs::core {

struct GetConfigOperation : ServiceOperationWithMetric<"CoreService", "GetConfig", "op"> {
  GetConfigReq req;

  explicit GetConfigOperation(GetConfigReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return "GetConfig"; }

  CoTryTask<GetConfigRsp> handle() {
    auto cfg = ApplicationBase::getConfigString(req.configKey);
    CO_RETURN_ON_ERROR(cfg);
    co_return GetConfigRsp::create(std::move(*cfg));
  }
};

}  // namespace hf3fs::core
