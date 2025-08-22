#include "MgmtdServiceStub.h"

#include "common/serde/ClientContext.h"
#include "common/serde/ClientMockContext.h"
#include "fbs/mgmtd/MgmtdServiceClient.h"

namespace hf3fs::mgmtd {
#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype) \
  template <typename Ctx>                                                       \
  CoTryTask<rsptype> svc##ServiceStub<Ctx>::name(const reqtype &req) {          \
    co_return co_await svc##ServiceClient<>::send<#name>(ctx_, req);            \
  }
#include "fbs/mgmtd/MgmtdServiceDef.h"

template class MgmtdServiceStub<serde::ClientContext>;
template class MgmtdServiceStub<serde::ClientMockContext>;
}  // namespace hf3fs::mgmtd
