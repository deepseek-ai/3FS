#pragma once

#include "common/app/ApplicationBase.h"
#include "common/utils/suicide.h"
#include "core/utils/ServiceOperation.h"
#include "fbs/core/service/Rpc.h"

namespace hf3fs::core {

struct ShutdownOperation : ServiceOperationWithMetric<"CoreService", "Shutdown", "op"> {
  ShutdownReq req_;

  explicit ShutdownOperation(ShutdownReq req)
      : req_(std::move(req)) {}

  String toStringImpl() const final { return "Shutdown"; }

  CoTryTask<ShutdownRsp> handle() {
    CO_RETURN_ON_ERROR(suicide(req_.graceful));
    co_return ShutdownRsp::create();
  }
};

}  // namespace hf3fs::core
