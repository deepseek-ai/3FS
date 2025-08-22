#include <chrono>
#include <folly/Random.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
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
#include "common/net/ib/IBDevice.h"
#include "common/net/sync/Client.h"
#include "common/serde/ClientContext.h"
#include "common/utils/Address.h"
#include "common/utils/Coroutine.h"
#include "tests/GtestHelpers.h"
#include "tests/common/net/Echo.h"
#include "tests/common/net/ib/SetupIB.h"

DEFINE_uint32(test_echo_loops, 1000, "run echo RPC times per coroutine in TestEcho.MultiThreads");
DEFINE_uint32(test_echo_threads, 8, "threads number in TestEcho.MultiThreads");

namespace hf3fs::net::test {
namespace {

class EchoServiceImpl : public serde::ServiceWrapper<EchoServiceImpl, Echo> {
 public:
  CoTryTask<EchoRsp> echo(serde::CallContext &ctx, const EchoReq &req) {
    EchoRsp rsp;
    rsp.val = req.val;
    co_return rsp;
  }

  CoTryTask<HelloRsp> hello(serde::CallContext &ctx, const HelloReq &req) {
    HelloRsp rsp;
    rsp.val = "Hello, " + req.val;
    rsp.idx = ++idx_;
    co_return rsp;
  }

  CoTryTask<HelloRsp> fail(serde::CallContext &ctx, const HelloReq &) {
    fmt::print("Request from {}\n", ctx.transport()->describe());
    co_return makeError(RPCCode::kInvalidMessageType, "failed");
  }

 private:
  uint32_t idx_ = 0;
};
static_assert(EchoServiceImpl::kServiceID == 86, "check service id failed");

using namespace std::chrono_literals;

class TestEcho : public testing::TestWithParam<std::tuple<Address::Type, bool>> {
 public:
  static void SetUpTestSuite() { SetupIB::SetUpTestSuite(); }

  void TearDown() override {
    client_.stopAndJoin();
    server_.stopAndJoin();
  }

  Address serverAddr() const { return server_.groups().front()->addressList().front(); }

 protected:
  Client::Config clientConfig_;
  bool initClientConfig = [this] {
    auto rwInEventThread = std::get<1>(GetParam());
    clientConfig_.io_worker().set_read_write_rdma_in_event_thread(rwInEventThread);
    clientConfig_.io_worker().set_read_write_tcp_in_event_thread(rwInEventThread);
    return true;
  }();
  Client client_{clientConfig_};

  Server::Config serverConfig_;
  bool initServerConfig = [this] {
    serverConfig_.groups(0).set_network_type(std::get<0>(GetParam()));

    auto rwInEventThread = std::get<1>(GetParam());
    serverConfig_.groups(0).io_worker().set_read_write_rdma_in_event_thread(rwInEventThread);
    serverConfig_.groups(0).io_worker().set_read_write_tcp_in_event_thread(rwInEventThread);
    return true;
  }();
  Server server_{serverConfig_};
};

TEST_P(TestEcho, RequestAndResponse) {
  server_.addSerdeService(std::make_unique<EchoServiceImpl>());
  ASSERT_TRUE(server_.setup());
  ASSERT_TRUE(server_.start());
  client_.start();
  auto ctx = client_.serdeCtx(serverAddr());

  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    Echo<> client;

    {
      EchoReq req;
      req.val = "foo";
      auto result = co_await client.echo(ctx, req);
      if (result.hasError()) {
        std::cout << result.error().describe() << std::endl;
      }
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->val, "foo");
    }

    {
      HelloReq req;
      req.val = "foo";
      auto result = co_await client.hello(ctx, req);
      CO_ASSERT_OK(result);

      CO_ASSERT_EQ(result->val, "Hello, foo");
      CO_ASSERT_EQ(result->idx, 1);

      result = co_await client.hello(ctx, req);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->idx, 2);
    }

    {
      EchoReq req;
      req.val = "foo";
      auto result = co_await client.echo(ctx, req);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->val, "foo");
    }

    {
      HelloReq req;
      req.val = "foo";
      auto result = co_await client.fail(ctx, req);
      CO_ASSERT_TRUE(result.hasError());
      CO_ASSERT_EQ(result.error().code(), RPCCode::kInvalidMessageType);
      CO_ASSERT_EQ(result.error().message(), "failed");
    }
  }));
}

TEST_P(TestEcho, AddrType) {
  server_.addSerdeService(std::make_unique<EchoServiceImpl>());
  ASSERT_TRUE(server_.setup());
  ASSERT_TRUE(server_.start());
  client_.start();
  auto addr = serverAddr();
  addr.type = addr.isTCP() ? Address::RDMA : Address::TCP;
  auto ctx = client_.serdeCtx(addr);

  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    Echo<> client;

    {
      EchoReq req;
      req.val = "foo";
      auto result = co_await client.echo(ctx, req);
      if (addr.isTCP()) {
        CO_ASSERT_TRUE(result.hasValue());
      } else {
        CO_ASSERT_FALSE(result.hasValue());
      }
    }
  }));
}

TEST_P(TestEcho, TimeoutStopped) {
  // Timeout caused by a stopped service
  server_.addSerdeService(std::make_unique<EchoServiceImpl>());
  ASSERT_TRUE(server_.setup());
  ASSERT_TRUE(server_.start());
  client_.start();
  net::UserRequestOptions options{};
  options.timeout = 100_ms;
  auto ctx = client_.serdeCtx(serverAddr());

  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    Echo<> client;
    server_.stopAndJoin();

    {
      EchoReq req;
      req.val = "foo";
      auto result = co_await client.echo(ctx, req, &options);
      CO_ASSERT_TRUE(result.hasError());
      CO_ASSERT_EQ(result.error().code(), RPCCode::kTimeout);
    }
  }));
}

TEST_P(TestEcho, WithTimestamp) {
  server_.addSerdeService(std::make_unique<EchoServiceImpl>());
  ASSERT_TRUE(server_.setup());
  ASSERT_TRUE(server_.start());
  client_.start();
  auto ctx = client_.serdeCtx(serverAddr());
  serde::Timestamp timestamp;

  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    Echo<> client;

    EchoReq req;
    req.val = "foo";
    auto result = co_await client.echo(ctx, req, nullptr, &timestamp);
    if (result.hasError()) {
      XLOGF(CRITICAL, "error: {}", result.error().describe());
    }
    CO_ASSERT_OK(result);
    CO_ASSERT_EQ(result->val, "foo");
  }));

  ASSERT_NE(timestamp.serverLatency(), Duration{});
}

TEST_P(TestEcho, Frozen) {
  server_.addSerdeService(std::make_unique<EchoServiceImpl>());
  ASSERT_TRUE(server_.setup());
  ASSERT_TRUE(server_.start());
  client_.start();
  auto ctx = client_.serdeCtx(serverAddr());

  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    Echo<> client;

    server_.setFrozen(true);
    {
      EchoReq req;
      req.val = "foo";
      auto result = co_await client.echo(ctx, req);
      CO_ASSERT_TRUE(result.hasError());
      CO_ASSERT_EQ(result.error().code(), RPCCode::kTimeout);
    }

    server_.setFrozen(false);
    {
      EchoReq req;
      req.val = "foo";
      auto result = co_await client.echo(ctx, req);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->val, "foo");
    }
  }));
}

TEST_P(TestEcho, SendFailed) {
  client_.start();
  net::UserRequestOptions options;
  options.timeout = 1_s;
  auto ctx = client_.serdeCtx(Address{0});

  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    Echo<> client;

    {
      EchoReq req;
      req.val = "foo";
      auto result = co_await client.echo(ctx, req, &options);
      CO_ASSERT_TRUE(result.hasError());
      CO_ASSERT_EQ(result.error().code(), RPCCode::kSendFailed);
    }
  }));
}

TEST_P(TestEcho, LargeMessage) {
  server_.addSerdeService(std::make_unique<EchoServiceImpl>());
  ASSERT_TRUE(server_.setup());
  ASSERT_TRUE(server_.start());
  client_.start();
  net::UserRequestOptions options;
  options.timeout = 5_s;
  auto ctx = client_.serdeCtx(serverAddr());

  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    Echo<> client;

    EchoReq req;
    req.val = std::string(10_MB + 1, 'A');
    auto result = co_await client.echo(ctx, req, &options);
    if (result.hasError()) {
      XLOGF(CRITICAL, "error: {}", result.error().describe());
    }
    CO_ASSERT_OK(result);
    CO_ASSERT_EQ(result->val, req.val);
  }));
}

TEST_P(TestEcho, CompressedLargeMessage) {
  server_.addSerdeService(std::make_unique<EchoServiceImpl>());
  ASSERT_TRUE(server_.setup());
  ASSERT_TRUE(server_.start());
  client_.start();
  net::UserRequestOptions options;
  options.timeout = 5_s;
  options.compression = {1, 1_KB};
  auto ctx = client_.serdeCtx(serverAddr());

  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    Echo<> client;

    EchoReq req;
    req.val = std::string(10_MB + 1, 'A');
    auto result = co_await client.echo(ctx, req, &options);
    if (result.hasError()) {
      XLOGF(CRITICAL, "error: {}", result.error().describe());
    }
    CO_ASSERT_OK(result);
    CO_ASSERT_EQ(result->val, req.val);
  }));
}

TEST_P(TestEcho, MultiThreads) {
  server_.addSerdeService(std::make_unique<EchoServiceImpl>());
  ASSERT_TRUE(server_.setup());
  ASSERT_TRUE(server_.start());
  // some CI machine enable kmemleak test, may case long delay.
  clientConfig_.set_default_timeout(10000_ms);
  client_.start();
  net::UserRequestOptions options;
  options.timeout = 5_s;
  auto ctx = client_.serdeCtx(serverAddr());

  auto testCoro = [&]() -> CoTask<bool> {
    for (size_t i = 0; i < FLAGS_test_echo_loops; i++) {
      Echo<> client;

      EchoReq req;
      req.val = std::to_string(folly::Random::rand32());
      auto result = co_await client.echo(ctx, req, &options);
      if (result.hasError()) {
        XLOGF(CRITICAL, "error: {}", result.error().describe());
        co_return false;
      }
      EXPECT_EQ(result->val, req.val);
    }
    co_return true;
  };

  for (size_t numThreads = 2; numThreads <= FLAGS_test_echo_threads; numThreads *= 2) {
    folly::CPUThreadPoolExecutor exec(numThreads);
    std::vector<folly::SemiFuture<bool>> tasks;
    for (size_t coroId = 0; coroId < 16 * numThreads; coroId++) {
      tasks.emplace_back(testCoro().scheduleOn(folly::Executor::getKeepAliveToken(exec)).start());
    }
    folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
      std::vector<bool> results = co_await folly::coro::collectAllRange(std::move(tasks));
      CO_ASSERT_TRUE(std::all_of(results.begin(), results.end(), [](auto ok) { return ok; }));
    }));
    exec.join();
  }
}

INSTANTIATE_TEST_SUITE_P(TestEcho,
                         TestEcho,
                         testing::Combine(testing::Values(Address::RDMA, Address::TCP, Address::UNIX),
                                          testing::Values(true, false)));

TEST(TestEchoSync, Normal) {
  std::array<Address::Type, 4> networks{Address::LOCAL, Address::TCP /*, Address::IPoIB */};
  for (auto network : networks) {
    Server::Config serverConfig;
    serverConfig.groups(0).set_network_type(network);
    Server server_{serverConfig};

    server_.addSerdeService(std::make_unique<EchoServiceImpl>());
    ASSERT_OK(server_.setup());
    auto initResult = server_.start();
    if (!initResult) {
      fmt::print("init failed: {}\n", initResult.error().describe());
      ASSERT_TRUE(initResult);
    }
    auto serverAddr = server_.groups().front()->addressList().front();

    sync::Client::Config clientConfig;
    sync::Client client_{clientConfig};
    auto ctx = client_.serdeCtx(serverAddr);
    Echo<> client;

    EchoReq req;
    req.val = "foo";
    auto result = client.echoSync(ctx, req);
    if (result.hasError()) {
      std::cout << result.error().describe() << std::endl;
    }
    ASSERT_FALSE(result.hasError());

    auto &rsp = result.value();
    ASSERT_EQ(rsp.val, "foo");

    req.val = "bar";
    result = client.echoSync(ctx, req);
    if (result.hasError()) {
      std::cout << result.error().describe() << std::endl;
    }
    ASSERT_FALSE(result.hasError());
    ASSERT_EQ(rsp.val, "bar");
  }
}

TEST(TestEchoListener, Normal) {
  Server::Config serverConfig;
  serverConfig.groups(0).set_network_type(Address::TCP);
  serverConfig.groups(0).listener().set_listen_port(folly::Random::rand32(10000, 20000));  // same port.
  Server server1{serverConfig};
  Server server2{serverConfig};
  ASSERT_OK(server1.setup());
  ASSERT_FALSE(server2.setup());
  ASSERT_OK(server1.start());
  ASSERT_FALSE(server2.start());
}

TEST(TestEchoListener, StartAndStop) {
  constexpr auto N = 100;
  for (auto i = 0; i < N; ++i) {
    Server::Config serverConfig;
    serverConfig.groups(0).set_network_type(Address::TCP);
    Server server{serverConfig};
    ASSERT_OK(server.setup());
    server.stopAndJoin();
  }
}

TEST(TestEchoListener, DomainSocket) {
  Server::Config serverConfig;
  serverConfig.groups(0).set_network_type(Address::UNIX);
  serverConfig.groups(0).listener().set_listen_port(folly::Random::rand32(10000, 20000));  // same port.
  Server server{serverConfig};
  ASSERT_OK(server.setup());
  ASSERT_OK(server.start());
  std::this_thread::sleep_for(1_s);
  server.stopAndJoin();
}

}  // namespace
}  // namespace hf3fs::net::test
