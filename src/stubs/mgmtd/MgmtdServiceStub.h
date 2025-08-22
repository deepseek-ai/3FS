#pragma once

#include "IMgmtdServiceStub.h"

namespace hf3fs::mgmtd {
template <typename Ctx>
class MgmtdServiceStub : public IMgmtdServiceStub {
 public:
  using ContextType = Ctx;
  explicit MgmtdServiceStub(ContextType ctx)
      : ctx_(std::move(ctx)) {}

#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype) \
  CoTryTask<rsptype> name(const reqtype &req) override;

#include "fbs/mgmtd/MgmtdServiceDef.h"

 private:
  ContextType ctx_;
};
}  // namespace hf3fs::mgmtd
