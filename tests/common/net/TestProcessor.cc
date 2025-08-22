#include <atomic>
#include <cstdint>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/futures/Future.h>
#include <gtest/gtest.h>

#include "common/net/Client.h"
#include "common/net/Network.h"
#include "common/net/Server.h"
#include "common/serde/Service.h"
#include "tests/common/net/Echo.h"
#include "tests/common/net/ib/SetupIB.h"

namespace hf3fs::net::test {
namespace {

class EchoServiceImpl : public serde::ServiceWrapper<EchoServiceImpl, Echo> {
 public:
  CoTryTask<EchoRsp> echo(serde::CallContext &ctx, const EchoReq &) {
    co_await folly::coro::sleep(50ms);
    co_return EchoRsp{};
  }
  CoTryTask<HelloRsp> hello(serde::CallContext &ctx, const HelloReq &) { co_return HelloRsp{}; }
  CoTryTask<HelloRsp> fail(serde::CallContext &ctx, const HelloReq &) { co_return HelloRsp{}; }

  Status onError(Status status) {
    ++error_;
    return status;
  }

  uint32_t error() { return error_; }

 private:
  std::atomic<uint32_t> error_ = 0;
};

}  // namespace

class TestProcessor : public SetupIB {};

TEST_F(TestProcessor, TooManyProcessingRequests) {
  constexpr auto kMaxProcessingRequestsNum = 10;
  constexpr auto kConcurrentRequestsNum = 16;

  Server::Config serverConfig;
  serverConfig.groups(0).processor().set_max_processing_requests_num(kMaxProcessingRequestsNum);
  Server server(serverConfig);
  ASSERT_TRUE(server.setup());
  auto impl = std::make_unique<EchoServiceImpl>();
  auto ptr = impl.get();
  server.addSerdeService(std::move(impl));
  ASSERT_TRUE(server.start());
  ASSERT_EQ(ptr->error(), 0);

  Client::Config clientConfig;
  Client client{clientConfig};
  client.start();
  auto ctx = client.serdeCtx(server.groups().front()->addressList().front());

  folly::coro::blockingWait([&]() -> CoTask<void> {
    Echo<> client;
    std::vector<folly::SemiFuture<Result<EchoRsp>>> requests;
    requests.reserve(kConcurrentRequestsNum);
    EchoReq echoReq{};
    for (auto i = 0; i < kConcurrentRequestsNum; ++i) {
      requests.push_back(client.echo(ctx, echoReq).semi());
    }
    auto results = co_await folly::collectAll(std::move(requests));

    auto succ = 0;
    auto fail = 0;
    for (auto &result : results) {
      auto &rsp = *result;
      if (rsp.hasError()) {
        ++fail;
        CO_ASSERT_EQ(rsp.error().code(), RPCCode::kRequestRefused);
      } else {
        ++succ;
      }
    }

    CO_ASSERT_GE(succ, kMaxProcessingRequestsNum);
    CO_ASSERT_GT(fail, 0);
    CO_ASSERT_LE(fail, kConcurrentRequestsNum - kMaxProcessingRequestsNum);
    CO_ASSERT_NE(ptr->error(), 0);
    CO_ASSERT_EQ(ptr->error(), fail);

    auto [rsp, _] = co_await folly::coro::collectAll(client.echo(ctx, EchoReq{}), [&]() -> CoTask<void> {
      co_await folly::coro::sleep(10ms);
      server.stopAndJoin();
    }());
  }());
}

}  // namespace hf3fs::net::test
