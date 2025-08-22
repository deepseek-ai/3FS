#pragma once

#include "stubs/mgmtd/IMgmtdServiceStub.h"

#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype) \
  std::function<Result<rsptype>(const reqtype &)> name##Func;                   \
  DummyMgmtdServiceStub &set_##name##Func(auto &&func) {                        \
    name##Func = std::forward<decltype(func)>(func);                            \
    return *this;                                                               \
  }                                                                             \
  CoTryTask<rsptype> name(const reqtype &req) override {                        \
    if (name##Func) {                                                           \
      co_return name##Func(req);                                                \
    }                                                                           \
    co_return makeError(RPCCode::kConnectFailed);                               \
  }

namespace hf3fs::mgmtd {
struct DummyMgmtdServiceStub : public IMgmtdServiceStub {
#include "fbs/mgmtd/MgmtdServiceDef.h"
};
}  // namespace hf3fs::mgmtd
