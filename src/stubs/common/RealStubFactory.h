#pragma once

#include "IStubFactory.h"
#include "common/serde/ClientContext.h"

namespace hf3fs::stubs {
using ClientContextCreator = std::function<serde::ClientContext(net::Address)>;

template <template <typename Ctx> typename Stub>
requires requires { typename Stub<serde::ClientContext>::InterfaceType; }
class RealStubFactory : public IStubFactory<typename Stub<serde::ClientContext>::InterfaceType> {
 public:
  using StubType = Stub<serde::ClientContext>;
  using InterfaceType = typename StubType::InterfaceType;

  explicit RealStubFactory(ClientContextCreator creator)
      : creator_(std::move(creator)) {}

  std::unique_ptr<InterfaceType> create(net::Address addr) override {
    return std::make_unique<StubType>(creator_(addr));
  }

 private:
  ClientContextCreator creator_;
};
}  // namespace hf3fs::stubs
