#pragma once

#include "stubs/common/IStubFactory.h"
#include "stubs/core/ICoreServiceStub.h"

namespace hf3fs::client {
class CoreClient {
 public:
  using CoreStub = core::ICoreServiceStub;
  using CoreStubFactory = stubs::IStubFactory<CoreStub>;

  explicit CoreClient(std::unique_ptr<CoreStubFactory> stubFactory)
      : stubFactory_(std::move(stubFactory)) {}
  ~CoreClient() = default;

#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype)                                      \
  CoTryTask<core::rsptype> name(net::Address addr, const core::reqtype &req) {                                       \
    auto stub = stubFactory_->create(addr);                                                                          \
    co_return co_await stub->name(req);                                                                              \
  }                                                                                                                  \
                                                                                                                     \
  CoTryTask<core::rsptype> name(const std::vector<net::Address> &addrs, const core::reqtype &req) {                  \
    for (auto addr : addrs) {                                                                                        \
      auto stub = stubFactory_->create(addr);                                                                        \
      auto res = co_await stub->name(req);                                                                           \
      if (res || StatusCode::typeOf(res.error().code()) != StatusCodeType::RPC) co_return res;                       \
    }                                                                                                                \
    co_return makeError(RPCCode::kConnectFailed,                                                                     \
                        fmt::format("Try to call CoreService::{} failed, addrs: {}", #name, fmt::join(addrs, ","))); \
  }

#include "fbs/core/service/CoreServiceDef.h"

 private:
  std::unique_ptr<CoreStubFactory> stubFactory_;
};  // namespace hf3fs::client
}  // namespace hf3fs::client
