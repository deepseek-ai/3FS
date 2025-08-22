#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Sleep.h>
#include <tuple>
#include <type_traits>
#include <utility>

#include "common/net/Client.h"
#include "common/net/Server.h"
#include "common/net/Transport.h"
#include "common/net/WriteItem.h"
#include "common/net/sync/Client.h"
#include "common/serde/CallContext.h"
#include "common/serde/ClientMockContext.h"
#include "common/serde/MessagePacket.h"
#include "common/serde/Serde.h"
#include "common/serde/Service.h"
#include "common/serde/Services.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Reflection.h"
#include "common/utils/Thief.h"
#include "common/utils/TypeTraits.h"
#include "tests/GtestHelpers.h"
#include "tests/common/net/ib/SetupIB.h"

namespace hf3fs::serde::test {
namespace {

struct EchoReq {
  SERDE_STRUCT_FIELD(value, std::string{});
};
struct EchoRsp {
  SERDE_STRUCT_FIELD(value, std::string{});
};
struct FooReq {
  SERDE_STRUCT_FIELD(value, 0);
};
struct FooRsp {
  SERDE_STRUCT_FIELD(value, 0.0);
};
struct FourReq {
  SERDE_STRUCT_FIELD(value, 0);
};
struct FourRsp {
  SERDE_STRUCT_FIELD(value, 0);
};

SERDE_SERVICE(DemoService, 1) {
  SERDE_SERVICE_METHOD(echo, 1, EchoReq, EchoRsp);
  SERDE_SERVICE_METHOD(foo, 2, FooReq, FooRsp);
  SERDE_SERVICE_METHOD(three, 3, int, int);
  SERDE_SERVICE_METHOD(four, 4, FourReq, FourRsp);
};

struct ServiceImpl : public serde::ServiceWrapper<ServiceImpl, DemoService> {
  CoTryTask<EchoRsp> echo(const EchoReq &req) {
    cnt += 1;
    EchoRsp rsp;
    rsp.value = req.value;
    co_return rsp;
  }
  CoTryTask<FooRsp> foo(const FooReq &req) {
    cnt += 2;
    FooRsp rsp;
    rsp.value = req.value + 1;
    co_return rsp;
  }
  CoTryTask<int> three(int req) {
    cnt += 4;
    XLOG(ERR, "req is {}", req);
    co_return req * 2;
  }
  CoTryTask<FourRsp> four(const FourReq &req) {
    cnt += 8;
    FourRsp rsp;
    rsp.value = req.value * req.value;
    co_return rsp;
  }
  int cnt = 0;
};

static_assert(DemoService<>::kServiceName == "DemoService");
static_assert(DemoService<>::kServiceID == 1);
static_assert(refl::Helper::Size<ServiceImpl> == 4);
static_assert(!serde::SerdeType<ServiceImpl>);

struct FakeClient {
  template <NameWrapper kServiceName,
            NameWrapper kMethodName,
            class Req,
            class Rsp,
            uint16_t ServiceID,
            uint16_t MethodID>
  CoTryTask<Rsp> call(const Req &, auto...) {
    XLOGF(INFO, "call method {}:{}", ServiceID, MethodID);
    co_return Rsp{};
  }
};

class TestService : public net::test::SetupIB {};

TEST_F(TestService, Client) {
  FakeClient ctx;
  DemoService a;
  folly::coro::blockingWait(a.echo(ctx, EchoReq{}));
  folly::coro::blockingWait(a.foo(ctx, FooReq{}));
}

TEST_F(TestService, Server) {
  ServiceImpl impl;
  ASSERT_EQ(impl.cnt, 0);
  folly::coro::blockingWait((impl.*refl::Helper::FieldInfo<ServiceImpl, 0>::method)(EchoReq{}));
  ASSERT_EQ(impl.cnt, 1);
  folly::coro::blockingWait((impl.*refl::Helper::FieldInfo<ServiceImpl, 1>::method)(FooReq{}));
  ASSERT_EQ(impl.cnt, 3);

  auto call = [&impl](auto type) {
    using T = std::decay_t<decltype(type)>;
    if constexpr (std::is_same_v<T, std::nullptr_t>) {
      XLOGF(INFO, "not found!");
    } else {
      folly::coro::blockingWait((impl.*T::method)(typename T::ReqType{}));
    }
  };
  callByIdx<refl::Helper::FieldInfoList<ServiceImpl>>(call, 0);
  ASSERT_EQ(impl.cnt, 4);
  callByIdx<refl::Helper::FieldInfoList<ServiceImpl>>(call, 1);
  ASSERT_EQ(impl.cnt, 6);
  callByIdx<refl::Helper::FieldInfoList<ServiceImpl>>(call, 4);
  ASSERT_EQ(impl.cnt, 6);
}

struct FakeCallContext {
  Result<std::string> handle(const std::string &str) {
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(recv_, str));
    auto methodPointer = MethodExtractor<ServiceImpl, FakeCallContext>::get(recv_.methodId);
    if (LIKELY(methodPointer != nullptr)) {
      return (this->*methodPointer)();
    } else {
      XLOGF(INFO, "method not found!");
      return makeError(RPCCode::kInvalidMethodID);
    }
  }

  template <class FieldInfo>
  Result<std::string> call() {
    typename FieldInfo::ReqType req;
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(req, recv_.payload));
    auto rsp = folly::coro::blockingWait((impl_.*FieldInfo::method)(req));
    RETURN_AND_LOG_ON_ERROR(rsp);
    MessagePacket send(*rsp);
    return serde::serialize(send);
  }

  ServiceImpl impl_;
  MessagePacket<> recv_;
};

class FakeServer {
 public:
  template <NameWrapper kServiceName,
            NameWrapper kMethodName,
            class Req,
            class Rsp,
            uint16_t ServiceID,
            uint16_t MethodID>
  CoTryTask<Rsp> call(const Req &req, auto...) {
    XLOGF(INFO, "call method {}:{}", ServiceID, MethodID);
    MessagePacket send(req);
    send.serviceId = ServiceID;
    send.methodId = MethodID;
    auto bytes = serde::serialize(send);

    FakeCallContext ctx;
    auto result = ctx.handle(bytes);
    CO_RETURN_ON_ERROR(result);
    MessagePacket recv;
    CO_RETURN_ON_ERROR(serde::deserialize(recv, *result));
    Rsp rsp;
    CO_RETURN_ON_ERROR(serde::deserialize(rsp, recv.payload));
    co_return rsp;
  }
};

TEST_F(TestService, ClientToServer) {
  FakeServer ctx;
  DemoService a;

  {
    EchoReq req;
    req.value = "hello";
    auto result = folly::coro::blockingWait(a.echo(ctx, req));
    ASSERT_OK(result);
    ASSERT_EQ(req.value, result->value);
  }

  {
    FooReq req;
    req.value = 100;
    auto result = folly::coro::blockingWait(a.foo(ctx, req));
    ASSERT_OK(result);
    ASSERT_EQ(req.value + 1, result->value);
  }

  {
    auto req = 100;
    auto result = folly::coro::blockingWait(a.three(ctx, req));
    ASSERT_OK(result);
    ASSERT_EQ(req * 2, *result);
  }

  {
    FourReq req;
    req.value = 100;
    auto result = folly::coro::blockingWait(a.four(ctx, req));
    ASSERT_OK(result);
    ASSERT_EQ(req.value * req.value, result->value);
  }
}

static_assert(sizeof(CallContext::MethodType) == 16);

template <class Context = serde::CallContext>
struct RealService : public serde::ServiceWrapper<RealService<Context>, DemoService> {
  CoTryTask<EchoRsp> echo(Context &, const EchoReq &req) {
    cnt += 1;
    EchoRsp rsp;
    rsp.value = req.value;
    co_return rsp;
  }
  CoTryTask<FooRsp> foo(Context &, const FooReq &req) {
    cnt += 2;
    FooRsp rsp;
    rsp.value = req.value + 1;
    co_return rsp;
  }
  CoTryTask<int> three(Context &, const int &req) {
    cnt += 4;
    co_return req * 2;
  }
  CoTryTask<FourRsp> four(Context &, const FourReq &) {
    cnt += 8;
    co_await folly::coro::sleep(std::chrono::milliseconds(10));
    co_return makeError(StatusCode::kInvalidConfig);
  }
  std::atomic<int> cnt = 0;
};

TEST_F(TestService, Normal) {
  Services services;
  ASSERT_OK(services.addService(std::make_unique<RealService<>>(), false));

  for (auto o = 0; o < 2; ++o) {
    auto &service = services.getServiceById(RealService<>::kServiceID + o, false);
    ASSERT_NE(service.getter, nullptr);

    for (auto i = 0; i < 100; ++i) {
      if (o == 0 && 1 <= i && i <= 4) {
        ASSERT_NE(service.getter(i), nullptr);
      } else {
        ASSERT_EQ(service.getter(i), &CallContext::invalidId);
      }
    }
  }
}

TEST_F(TestService, AddSerdeService) {
  net::Server::Config config;
  net::Server server(config);
  ASSERT_OK(server.setup());
  ASSERT_OK(server.addSerdeService(std::make_unique<RealService<>>()));

  ASSERT_OK(server.start());
  server.stopAndJoin();
}

TEST_F(TestService, CallContext) {
  auto service = std::make_unique<RealService<>>();
  auto pointer = service.get();

  Services services;
  ASSERT_OK(services.addService(std::move(service), false));

  EchoReq req;
  req.value = "hello";
  MessagePacket send(req);
  send.serviceId = 1;
  send.methodId = 1;
  auto bytes = serde::serialize(send);

  MessagePacket<> recv;
  ASSERT_OK(serde::deserialize(recv, bytes));

  net::Server::Config config;
  net::Server server(config);
  ASSERT_OK(server.setup());
  ASSERT_OK(server.start());
  auto tr = net::Transport::create(net::Address{}, server.groups().front()->ioWorker());

  {
    ASSERT_EQ(pointer->cnt, 0);
    CallContext ctx(recv, tr, services.getServiceById(recv.serviceId, false));
    folly::coro::blockingWait(ctx.handle());
    ASSERT_EQ(pointer->cnt, 1);
  }

  {
    recv.methodId = 0;
    CallContext ctx(recv, tr, services.getServiceById(recv.serviceId, false));
    folly::coro::blockingWait(ctx.handle());
    ASSERT_EQ(pointer->cnt, 1);
  }

  {
    recv.serviceId = 0;
    CallContext ctx(recv, tr, services.getServiceById(recv.serviceId, false));
    folly::coro::blockingWait(ctx.handle());
    ASSERT_EQ(pointer->cnt, 1);
  }

  server.stopAndJoin();
}

TEST_F(TestService, ClientContext) {
  // 1. create service.
  auto service = std::make_unique<RealService<>>();
  auto pointer = service.get();

  // 2. start server.
  auto ctx = serde::ClientMockContext::create(std::move(service), net::Address::RDMA);

  // 4. call client ctx.
  {
    DemoService stub;
    EchoReq req{"hello"};
    auto result = folly::coro::blockingWait(stub.echo(ctx, req));
    ASSERT_OK(result);
    ASSERT_EQ(result->value, req.value);
    ASSERT_EQ(pointer->cnt, 1);
  }

  {
    DemoService stub;
    FooReq req{100};
    auto result = folly::coro::blockingWait(stub.foo(ctx, req));
    ASSERT_OK(result);
    ASSERT_EQ(result->value, req.value + 1);
    ASSERT_EQ(pointer->cnt, 3);
  }

  {
    DemoService stub;
    FooReq req{100};
    auto result = stub.fooSync(ctx, req);
    ASSERT_OK(result);
    ASSERT_EQ(result->value, req.value + 1);
    ASSERT_EQ(pointer->cnt, 5);
  }

  {
    DemoService stub;
    Timestamp timestamp{};
    auto result = folly::coro::blockingWait(stub.three(ctx, 100, nullptr, &timestamp));
    ASSERT_OK(result);
    ASSERT_EQ(*result, 200);
    ASSERT_EQ(pointer->cnt, 9);
    serde::iterate(
        [](std::string_view name, auto &value) {
          if (value == 0) {
            XLOGF(ERR, "{} is zero!", name);
            ASSERT_NE(value, 0);
          }
        },
        timestamp);
    XLOGF(INFO, "timestamp is {}", timestamp);
  }

  {
    DemoService stub;
    FourReq req{100};
    auto result = folly::coro::blockingWait(stub.four(ctx, req));
    ASSERT_FALSE(result);
    XLOGF(INFO, "result is error: {}", result.error());
  }

  {
    DemoService stub;
    FourReq req{100};
    net::UserRequestOptions options;
    options.timeout = 1_us;
    auto result = folly::coro::blockingWait(stub.four(ctx, req, &options));
    ASSERT_FALSE(result);
    XLOGF(INFO, "result is error: {}", result.error());
  }
}

TEST_F(TestService, CallSync) {
  using namespace hf3fs::net;

  // note: No IPoIB device in gitlab ci docker runner.
  std::array<Address::Type, 4> networks{Address::LOCAL, Address::TCP /*, Address::IPoIB */};
  for (auto network : networks) {
    Server::Config serverConfig;
    serverConfig.groups(0).set_network_type(network);
    Server server_{serverConfig};

    ASSERT_OK(server_.addSerdeService(std::make_unique<RealService<>>()));
    ASSERT_OK(server_.setup());
    ASSERT_OK(server_.start());
    auto serverAddr = server_.groups().front()->addressList().front();

    net::sync::Client::Config clientConfig;
    net::sync::Client client_{clientConfig};
    auto ctx = client_.serdeCtx(serverAddr);

    EchoReq req{"hello"};
    auto result = DemoService<>::echoSync(ctx, req);
    ASSERT_OK(result);
    ASSERT_EQ(result->value, req.value);

    server_.stopAndJoin();
  }
}

}  // namespace
}  // namespace hf3fs::serde::test
