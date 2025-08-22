#pragma once

#include <functional>

#include "common/serde/ClientContext.h"
#include "common/utils/RobinHood.h"
#include "stubs/common/Stub.h"

namespace hf3fs::stubs {

template <template <typename Ctx> typename StubImpl, bool SupportsStubStub = false>
class SerdeStubFactory {
 public:
  using Stub = stubs::SerdeStub<StubImpl, SupportsStubStub>;
  using MockStub = typename Stub::MockStub;
  using IStub = typename Stub::IStub;
  using MockContext = typename Stub::MockContext;
  using StubStub = typename Stub::StubStub;

  using SerdeContextCreator = std::function<serde::ClientContext(net::Address)>;
  explicit SerdeStubFactory(SerdeContextCreator creator)
      : ctx_(std::move(creator)) {}
  explicit SerdeStubFactory(serde::ClientMockContext context)
      : ctx_(std::move(context)) {}
  explicit SerdeStubFactory(robin_hood::unordered_map<net::Address, serde::ClientMockContext> contextMap)
      : ctx_(std::move(contextMap)) {}
  explicit SerdeStubFactory(MockContext ctx)
      : ctx_(std::move(ctx)) {}

  Stub create(net::Address addr, NodeId node = NodeId(0), std::string_view host = "mock") {
    struct MakeStub {
      MakeStub(net::Address addr, NodeId node, std::string_view host)
          : addr_(addr),
            node_(node),
            host_(host) {}
      Stub operator()(SerdeContextCreator &creator) { return Stub(creator(addr_), node_, host_); }
      Stub operator()(serde::ClientMockContext &ctx) { return Stub(ctx); }
      Stub operator()(robin_hood::unordered_map<net::Address, serde::ClientMockContext> &map) {
        return Stub(map[addr_], node_, host_);
      }
      Stub operator()(MockContext &ctx) {
        if constexpr (SupportsStubStub) {
          return Stub(ctx);
        } else {
          return Stub{};
        }
      }

     private:
      net::Address addr_;
      NodeId node_;
      std::string_view host_;
    };

    return std::visit(MakeStub(addr, node, host), ctx_);
  }

 private:
  std::conditional_t<SupportsStubStub,
                     std::variant<SerdeContextCreator,
                                  serde::ClientMockContext,
                                  robin_hood::unordered_map<net::Address, serde::ClientMockContext>,
                                  MockContext>,
                     std::variant<SerdeContextCreator,
                                  serde::ClientMockContext,
                                  robin_hood::unordered_map<net::Address, serde::ClientMockContext>>>
      ctx_;
};

}  // namespace hf3fs::stubs
