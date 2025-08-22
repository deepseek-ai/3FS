#include "MgmtdService.h"

#include "MgmtdOperator.h"

#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype)        \
  CoTryTask<rsptype> svc##Service::name(serde::CallContext &ctx, const reqtype &req) { \
    co_return co_await operator_.name(req, net::PeerInfo{ctx.peer()});                 \
  }

namespace hf3fs::mgmtd {
#include "fbs/mgmtd/MgmtdServiceDef.h"
}  // namespace hf3fs::mgmtd
