#pragma once

#include "common/app/ApplicationBase.h"
#include "core/utils/ServiceOperation.h"
#include "fbs/core/service/Rpc.h"

namespace hf3fs::core {

struct RenderConfigOperation : ServiceOperationWithMetric<"CoreService", "RenderConfig", "op"> {
  RenderConfigReq req;

  explicit RenderConfigOperation(RenderConfigReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return "RenderConfig"; }

  CoTryTask<RenderConfigRsp> handle() {
    auto res = ApplicationBase::renderConfig(req.configTemplate, req.testUpdate, req.isHotUpdate);
    CO_RETURN_ON_ERROR(res);
    auto &[content, updateRes] = *res;
    if (updateRes) {
      co_return RenderConfigRsp::create(std::move(content), Status(StatusCode::kOK), std::move(*updateRes));
    } else {
      co_return RenderConfigRsp::create(std::move(content), std::move(updateRes.error()));
    }
  }
};

}  // namespace hf3fs::core
