#include "CoreService.h"

#include "core/service/ops/Include.h"
#include "core/utils/runOp.h"

#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype)        \
  CoTryTask<rsptype> svc##Service::name(serde::CallContext &ctx, const reqtype &req) { \
    Name##Operation op(std::move(req));                                                \
    CO_INVOKE_OP_INFO(op, ctx.peer());                                                 \
  }

namespace hf3fs::core {
#include "fbs/core/service/CoreServiceDef.h"
}  // namespace hf3fs::core
