#include "CoreServiceStub.h"

#include "common/serde/ClientContext.h"
#include "common/serde/ClientMockContext.h"
#include "fbs/core/service/CoreServiceClient.h"

namespace hf3fs::core {
#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype) \
  template <typename Ctx>                                                       \
  CoTryTask<rsptype> svc##ServiceStub<Ctx>::name(const reqtype &req) {          \
    co_return co_await svc##ServiceClient<>::send<#name>(ctx_, req);            \
  }
#include "fbs/core/service/CoreServiceDef.h"

template class CoreServiceStub<serde::ClientContext>;
template class CoreServiceStub<serde::ClientMockContext>;
}  // namespace hf3fs::core
