#pragma once

#include "fbs/mgmtd/Rpc.h"

namespace hf3fs::mgmtd {

class IMgmtdServiceStub {
 public:
  using InterfaceType = IMgmtdServiceStub;

  virtual ~IMgmtdServiceStub() = default;

#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype) \
  virtual CoTryTask<rsptype> name(const reqtype &req) = 0;
#include "fbs/mgmtd/MgmtdServiceDef.h"
};
}  // namespace hf3fs::mgmtd
