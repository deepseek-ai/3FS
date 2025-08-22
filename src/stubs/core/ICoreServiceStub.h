#pragma once

#include "fbs/core/service/Rpc.h"

namespace hf3fs::core {

class ICoreServiceStub {
 public:
  using InterfaceType = ICoreServiceStub;

  virtual ~ICoreServiceStub() = default;

#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype) \
  virtual CoTryTask<rsptype> name(const reqtype &req) = 0;
#include "fbs/core/service/CoreServiceDef.h"
};
}  // namespace hf3fs::core
