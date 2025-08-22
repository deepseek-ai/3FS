#include <chrono>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "common/net/Client.h"
#include "common/net/Listener.h"
#include "common/net/Server.h"
#include "common/net/WriteItem.h"
#include "common/net/ib/IBDevice.h"
#include "common/net/sync/Client.h"
#include "common/serde/ClientContext.h"
#include "common/utils/Address.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "gtest/gtest.h"
#include "tests/GtestHelpers.h"
#include "tests/common/net/Echo.h"

namespace hf3fs::net {
using namespace test;

class TestIBNotInitialized : public ::testing::Test {
 protected:
  void SetUp() override { IBManager::stop(); }
};

TEST_F(TestIBNotInitialized, Basic) {
  ASSERT_FALSE(IBManager::initialized());
  ASSERT_TRUE(IBDevice::all().empty());
}

class EchoServiceImpl : public serde::ServiceWrapper<EchoServiceImpl, Echo> {
 public:
  CoTryTask<EchoRsp> echo(serde::CallContext &ctx, const EchoReq &req) {
    EchoRsp rsp;
    rsp.val = req.val;
    co_return rsp;
  }

  CoTryTask<HelloRsp> hello(serde::CallContext &ctx, const HelloReq &req) {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<HelloRsp> fail(serde::CallContext &ctx, const HelloReq &) {
    co_return makeError(StatusCode::kNotImplemented);
  }
};

TEST_F(TestIBNotInitialized, TCP) {
  Server::Config serverConfig;
  serverConfig.groups(0).set_network_type(Address::TCP);
  Server server{serverConfig};
  ASSERT_TRUE(server.setup());
  server.addSerdeService(std::make_unique<EchoServiceImpl>());
  ASSERT_TRUE(server.start());

  Client::Config clientConfig;
  Client client{clientConfig};
  client.start();

  auto ctx = client.serdeCtx(server.groups().front()->addressList().front());
  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    Echo<> client;
    EchoReq req;
    req.val = "foo";
    auto result = co_await client.echo(ctx, req);
    CO_ASSERT_OK(result);
    CO_ASSERT_EQ(result->val, "foo");
  }));
}

TEST_F(TestIBNotInitialized, RDMAServer) {
  Server::Config serverConfig;
  serverConfig.groups(0).set_network_type(Address::RDMA);
  Server server{serverConfig};
  ASSERT_OK(server.setup());
  ASSERT_ERROR(server.start(), RPCCode::kIBDeviceNotInitialized);
}

TEST_F(TestIBNotInitialized, RDMAClient) {
  Client::Config clientConfig;
  Client client{clientConfig};
  client.start();

  auto ctx = client.serdeCtx(Address::fromString("rdma://127.0.0.1:8000"));
  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    Echo<> client;
    EchoReq req;
    req.val = "foo";
    auto result = co_await client.echo(ctx, req);
    CO_ASSERT_TRUE(result.hasError());
    CO_ASSERT_EQ(result.error().code(), RPCCode::kSendFailed);
  }));
}

}  // namespace hf3fs::net
