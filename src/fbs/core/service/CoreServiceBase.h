#pragma once

#include "Rpc.h"
#include "common/serde/Service.h"

namespace hf3fs::core {
SERDE_SERVICE_2(CoreServiceBase, Core, 10001) {
#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype) \
  SERDE_SERVICE_METHOD(name, id, reqtype, rsptype);
#include "CoreServiceDef.h"
};
}  // namespace hf3fs::core
