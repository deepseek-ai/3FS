#pragma once

#include "common/serde/CallContext.h"
#include "fbs/core/service/CoreServiceBase.h"

namespace hf3fs::core {
class CoreService : public serde::ServiceWrapper<CoreService, CoreServiceBase> {
 public:
#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype) \
  CoTryTask<rsptype> name(serde::CallContext &ctx, const reqtype &req);

#include "fbs/core/service/CoreServiceDef.h"
};
}  // namespace hf3fs::core
