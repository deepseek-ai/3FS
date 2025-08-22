#pragma once

#include "common/serde/CallContext.h"
#include "fbs/mgmtd/MgmtdServiceBase.h"

namespace hf3fs::mgmtd {
class MgmtdOperator;
class MgmtdService : public serde::ServiceWrapper<MgmtdService, MgmtdServiceBase> {
 public:
  MgmtdService(MgmtdOperator &opr)
      : operator_(opr) {}

#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype) \
  CoTryTask<rsptype> name(serde::CallContext &ctx, const reqtype &req);

#include "fbs/mgmtd/MgmtdServiceDef.h"

 private:
  MgmtdOperator &operator_;
};
}  // namespace hf3fs::mgmtd
