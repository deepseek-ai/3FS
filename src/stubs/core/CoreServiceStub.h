#pragma once

#include "ICoreServiceStub.h"

namespace hf3fs::core {
template <typename Ctx>
class CoreServiceStub : public ICoreServiceStub {
 public:
  using ContextType = Ctx;
  explicit CoreServiceStub(ContextType ctx)
      : ctx_(std::move(ctx)) {}

#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype) \
  CoTryTask<rsptype> name(const reqtype &req) override;

#include "fbs/core/service/CoreServiceDef.h"

 private:
  ContextType ctx_;
};
}  // namespace hf3fs::core
