#pragma once

#include "Rpc.h"
#include "common/serde/Service.h"

namespace hf3fs::mgmtd {
SERDE_SERVICE_2(MgmtdServiceBase, Mgmtd, 217) {
#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype) \
  SERDE_SERVICE_METHOD_REFL(name, id, reqtype, rsptype);
#include "MgmtdServiceDef.h"
};
}  // namespace hf3fs::mgmtd
