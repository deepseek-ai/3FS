#pragma once

#include <string>
#include <variant>

#include "common/serde/ClientContext.h"
#include "common/serde/ClientMockContext.h"
#include "fbs/mgmtd/MgmtdTypes.h"

namespace hf3fs::stubs {
using flat::NodeId;

template <typename IStub>
struct StubMockContext {};

template <template <typename Ctx> typename StubImpl, bool SupportsStubStub = false>
class SerdeStub {
 public:
  using RealStub = StubImpl<serde::ClientContext>;
  using MockStub = StubImpl<serde::ClientMockContext>;
  using IStub = typename RealStub::InterfaceType;
  using MockContext = std::conditional_t<SupportsStubStub, stubs::StubMockContext<IStub>, int>;
  using StubStub = std::conditional_t<SupportsStubStub, StubImpl<MockContext>, void>;

  template <typename Ctx>
  SerdeStub(Ctx ctx, NodeId node = NodeId(0), std::string_view host = "mock")
      : nodeId(node),
        hostname(host),
        stub_{std::in_place_type<StubImpl<Ctx>>, std::move(ctx)} {}

  IStub *operator->() { return get(); }

  IStub *get() {
    return std::visit([](auto &v) -> IStub * { return &v; }, stub_);
  }

 public:
  NodeId nodeId{0};
  std::string hostname;

 private:
  std::conditional_t<SupportsStubStub, std::variant<RealStub, MockStub, StubStub>, std::variant<RealStub, MockStub>>
      stub_;
};

}  // namespace hf3fs::stubs
