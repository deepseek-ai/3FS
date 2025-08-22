#pragma once

#include "common/app/ApplicationBase.h"
#include "core/utils/ServiceOperation.h"
#include "fbs/core/service/Rpc.h"

namespace hf3fs::core {

struct HotUpdateConfigOperation : ServiceOperationWithMetric<"CoreService", "HotUpdateConfig", "op"> {
  HotUpdateConfigReq req;

  explicit HotUpdateConfigOperation(HotUpdateConfigReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return "HotUpdateConfig"; }

  CoTryTask<HotUpdateConfigRsp> handle() {
    auto res = ApplicationBase::hotUpdateConfig(req.update, req.render);
    CO_RETURN_ON_ERROR(res);
    co_return HotUpdateConfigRsp::create();
  }
};

}  // namespace hf3fs::core
