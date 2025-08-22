#include <algorithm>
#include <chrono>
#include <folly/Executor.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/futures/Future.h>
#include <folly/logging/LogConfig.h>
#include <folly/logging/LogConfigParser.h>
#include <folly/logging/Logger.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/ObjectToString.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "common/net/Client.h"
#include "common/net/Listener.h"
#include "common/net/Server.h"
#include "common/net/ib/IBConnectService.h"
#include "common/net/ib/IBDevice.h"
#include "common/net/ib/RDMABuf.h"
#include "common/net/sync/Client.h"
#include "common/serde/ClientContext.h"
#include "common/serde/Serde.h"
#include "common/serde/Service.h"
#include "common/utils/Address.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "tests/GtestHelpers.h"
#include "tests/common/net/ib/SetupIB.h"

DEFINE_bool(test_rdma_disable_log, false, "disable in TestRDMA");
DEFINE_double(test_rdma_fault_injection_rate, 0.001, "fault injection rate in TestRDMA");
DEFINE_int32(test_rdma_connections, 4, "connections in TestRDMA");
DEFINE_int32(test_rdma_pkey_index, 0, "pkey_index in TestRDMA");
namespace hf3fs::net {

struct RDMAReq {
  SERDE_STRUCT_FIELD(bufs, std::vector<RDMARemoteBuf>{});
  SERDE_STRUCT_FIELD(val, uint64_t(0));
};

struct RDMARsp {
  SERDE_STRUCT_FIELD(dummy, Void());
};

SERDE_SERVICE(RDMAService, 87) { SERDE_SERVICE_METHOD(test, 1, RDMAReq, RDMARsp); };

class RDMAServiceImpl : public serde::ServiceWrapper<RDMAServiceImpl, RDMAService> {
 public:
  RDMAServiceImpl()
      : bufPool_() {
    bufPool_ = net::RDMABufPool::create(1 << 20, 1024);
  }

  CoTryTask<RDMARsp> test(serde::CallContext &ctx, const RDMAReq &req) {
    RDMARsp rsp;
    auto *ibsocket = ctx.transport()->ibSocket();
    XLOGF_IF(FATAL, ibsocket == nullptr, "Not IBSocket");

    co_await folly::coro::sleep(std::chrono::milliseconds(folly::Random::rand32(100)));
    if (!req.bufs.empty()) {
      auto buf = co_await bufPool_->allocate();
      if (!buf) {
        co_return makeError(RPCCode::kRDMANoBuf);
      }
      auto batch = ibsocket->rdmaWriteBatch();
      for (auto remote : req.bufs) {
        if (buf.size() < remote.size()) {
          auto result = co_await batch.post();
          if (result.hasError()) {
            co_return makeError(std::move(result.error()));
          }
          batch.clear();
          buf.resetRange();
        }
        auto subbuf = buf.takeFirst(remote.size());
        if (!subbuf) {
          EXPECT_TRUE(false);
          co_return makeError(StatusCode::kInvalidArg);
        }
        XLOGF_IF(FATAL, req.val == 0, "here");
        *(uint64_t *)subbuf.ptr() = req.val;
        if (subbuf.size() >= 16) {
          *(uint64_t *)(subbuf.ptr() + subbuf.size() - 8) = req.val;
        }
        batch.add(remote, subbuf);
      }
      auto result = co_await batch.post();
      if (result.hasError()) {
        co_return makeError(std::move(result.error()));
      }
    }
    co_return rsp;
  }

 private:
  std::shared_ptr<net::RDMABufPool> bufPool_;
};

using namespace std::chrono_literals;

class TestRDMA : public test::SetupIB {
 public:
  void SetUp() override {
    auto &logger = folly::LoggerDB::get();
    if (FLAGS_test_rdma_disable_log) {
      oldLogConfig_ = logger.getConfig();
      logger.updateConfig(folly::parseLogConfig("CRITICAL"));
    }

    server_ = std::make_unique<Server>(serverConfig_);
    server_->addSerdeService(std::make_unique<RDMAServiceImpl>());
    ASSERT_TRUE(server_->setup());
    ASSERT_TRUE(server_->start());
    ASSERT_TRUE(client_.start());

    clientConfig_.io_worker().ibsocket().set_pkey_index(FLAGS_test_rdma_pkey_index);
  }
  void TearDown() override {
    if (FLAGS_test_rdma_disable_log) {
      folly::LoggerDB::get().updateConfig(oldLogConfig_);
    }
    client_.stopAndJoin();
    if (server_) {
      server_->stopAndJoin();
      server_.reset();
    }
  }

  Address serverAddr() const { return server_->groups().front()->addressList().front(); }

  void serverRestart() {
    if (server_) {
      serverConfig_.groups(0).listener().set_listen_port(serverAddr().port);
      server_->stopAndJoin();
      corpse_.push_back(std::move(server_));
    }
    server_ = std::make_unique<Server>(serverConfig_);
    server_->addSerdeService(std::make_unique<RDMAServiceImpl>());
    ASSERT_TRUE(server_->setup());
    ASSERT_TRUE(server_->start());
  }

  void serverDropConnections() {
    for (auto &grp : server_->groups()) {
      grp->ioWorker().dropConnections(true, true);
    }
  }

 protected:
  Client::Config clientConfig_;
  bool initClientConfig = [this] {
    clientConfig_.io_worker().transport_pool().set_max_connections(FLAGS_test_rdma_connections);
    clientConfig_.io_worker().ibsocket().set_max_rdma_wr(16);
    clientConfig_.io_worker().ibsocket().set_max_rdma_wr_per_post(4);
    clientConfig_.io_worker().set_num_event_loop(2);
    clientConfig_.thread_pool().set_num_connect_threads(2);
    clientConfig_.thread_pool().set_num_io_threads(4);
    clientConfig_.thread_pool().set_num_proc_threads(4);
    clientConfig_.set_default_timeout(2_s);
    return true;
  }();
  Client client_{clientConfig_};

  Server::Config serverConfig_;
  bool initServerConfig = [this] {
    serverConfig_.groups(0).set_network_type(Address::RDMA);
    serverConfig_.thread_pool().set_num_io_threads(4);
    serverConfig_.thread_pool().set_num_proc_threads(4);
    return true;
  }();
  std::unique_ptr<Server> server_;
  std::vector<std::unique_ptr<Server>> corpse_;

  folly::LogConfig oldLogConfig_;
};

CoTask<void> runClient(RDMAService<> client,
                       serde::ClientContext ctx,
                       size_t bufSize,
                       size_t bufCnt,
                       bool injectFault,
                       bool allowError,
                       Duration dur,
                       net::UserRequestOptions opts = {}) {
  auto invalidMR = net::RDMABuf::allocate(bufSize).toRemoteBuf();
  auto buf = net::RDMABuf::allocate(bufSize * bufCnt);
  CO_ASSERT_TRUE(buf);
  std::vector<net::RDMABuf> rdmaBufs;
  for (size_t i = 0; i < bufCnt; i++) {
    auto subBuf = buf.takeFirst(bufSize);
    CO_ASSERT_TRUE(subBuf);
    rdmaBufs.push_back(subBuf);
  }
  auto begin = SteadyClock::now();
  do {
    // call RPCs here
    auto val = folly::Random::rand64();
    RDMAReq req;
    req.val = val;

    XLOGF(DBG, "request {}", req.val);

    // use random number of buf
    size_t numBuf = folly::Random::rand32(rdmaBufs.size());
    for (size_t bufIdx = 0; bufIdx < numBuf; bufIdx++) {
      // buf with random length
      auto buf = rdmaBufs.at(bufIdx);
      auto len = folly::Random::rand32(8, buf.size() + 1);
      auto subbuf = buf.first(len);
      CO_ASSERT_TRUE(subbuf);
      *(uint64_t *)subbuf.ptr() = 0;
      if (subbuf.size() >= 16) {
        *(uint64_t *)(subbuf.ptr() + subbuf.size() - 8) = 0;
      }
      auto mr = subbuf.toRemoteBuf();
      if (injectFault && folly::Random::randDouble01() < FLAGS_test_rdma_fault_injection_rate) {
        mr = invalidMR.first(mr.size());
      }
      req.bufs.push_back(mr);
    }
    std::shuffle(req.bufs.begin(), req.bufs.end(), std::mt19937());

    auto result = co_await client.test(ctx, req, &opts);
    if (!allowError) {
      CO_ASSERT_OK(result);
    }
    if (!result.hasError()) {
      for (auto subbuf : req.bufs) {
        // check data
        CO_ASSERT_EQ(*(uint64_t *)subbuf.addr(), val);
        if (subbuf.size() >= 16) {
          CO_ASSERT_EQ(*(uint64_t *)(subbuf.addr() + subbuf.size() - 8), val);
        }
      }
    } else {
      XLOGF(DBG, "req {}, error {}", req.val, result.error());
    }
  } while (SteadyClock::now() - begin <= dur);
}

CoTask<void> startMultiClient(folly::Executor::KeepAlive<> exec,
                              size_t nClients,
                              RDMAService<> client,
                              serde::ClientContext ctx,
                              size_t bufSize,
                              size_t bufCnt,
                              bool injectFault,
                              bool allowError,
                              Duration dur,
                              net::UserRequestOptions opts = {}) {
  std::vector<folly::SemiFuture<Void>> tasks;
  for (size_t i = 0; i < nClients; i++) {
    tasks.push_back(
        runClient(client, ctx, bufSize, bufCnt, injectFault, allowError, dur, opts).scheduleOn(exec).start());
  }
  co_await folly::coro::collectAllRange(std::move(tasks));
}

TEST_F(TestRDMA, Basic) {
  auto ctx = client_.serdeCtx(serverAddr());

  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    RDMAService<> client;
    co_await runClient(client, ctx, 512 << 10, 8, false, false, 5_s);
  }));
}

TEST_F(TestRDMA, Concurrent) {
  auto ctx = client_.serdeCtx(serverAddr());

  folly::CPUThreadPoolExecutor exec(8);
  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    RDMAService<> client;
    co_await startMultiClient(&exec, 128, client, ctx, 512 << 10, 8, false, false, 10_s);
  }));
}

TEST_F(TestRDMA, ShortTimeout) {
  auto ctx = client_.serdeCtx(serverAddr());

  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    RDMAService<> client;
    net::UserRequestOptions opt;
    opt.timeout = 100_ms;
    co_await runClient(client, ctx, 512 << 10, 8, false, true, 5_s, opt);
  }));
}

TEST_F(TestRDMA, ShortTimeoutConcurrent) {
  auto ctx = client_.serdeCtx(serverAddr());

  folly::CPUThreadPoolExecutor exec(8);
  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    RDMAService<> client;
    net::UserRequestOptions opt;
    opt.timeout = 100_ms;
    co_await startMultiClient(&exec, 128, client, ctx, 512 << 10, 8, false, true, 10_s, opt);
  }));
}

TEST_F(TestRDMA, FaultInject) {
  auto ctx = client_.serdeCtx(serverAddr());

  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    RDMAService<> client;
    co_await runClient(client, ctx, 512 << 10, 8, true, true, 5_s);
    co_await runClient(client, ctx, 512 << 10, 8, false, true, 5_s);
    co_await runClient(client, ctx, 512 << 10, 8, false, false, 5_s);
  }));
}

TEST_F(TestRDMA, FaultInjectConcurrent) {
  auto ctx = client_.serdeCtx(serverAddr());

  folly::CPUThreadPoolExecutor exec(8);
  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    RDMAService<> client;
    co_await startMultiClient(&exec, 32, client, ctx, 512 << 10, 8, true, true, 5_s);
    co_await startMultiClient(&exec, 32, client, ctx, 512 << 10, 8, false, true, 5_s);
    co_await startMultiClient(&exec, 32, client, ctx, 512 << 10, 8, false, false, 5_s);
  }));
}

TEST_F(TestRDMA, SlowConnect) {
  IBConnectService::delayMs() = 200;
  SCOPE_EXIT { IBConnectService::delayMs() = 0; };
  auto ctx = client_.serdeCtx(serverAddr());

  folly::CPUThreadPoolExecutor exec(8);
  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    RDMAService<> client;
    net::UserRequestOptions opt;
    opt.timeout = 200_ms;
    co_await runClient(client, ctx, 512 << 10, 8, false, true, 5_s, opt);
    co_await startMultiClient(&exec, 128, client, ctx, 512 << 10, 8, false, true, 10_s, opt);
  }));
}

TEST_F(TestRDMA, ConnectitonLost) {
  IBConnectService::connectionLost() = true;
  SCOPE_EXIT { IBConnectService::connectionLost() = false; };
  auto ctx = client_.serdeCtx(serverAddr());

  folly::CPUThreadPoolExecutor exec(8);
  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    RDMAService<> client;
    net::UserRequestOptions opt;
    co_await runClient(client, ctx, 512 << 10, 8, false, true, 100_ms, opt);
    co_await folly::coro::sleep(std::chrono::seconds(20));
  }));
}

TEST_F(TestRDMA, ServerDropConnections) {
  auto opts = net::UserRequestOptions{};
  opts.sendRetryTimes = 0;
  auto ctx = client_.serdeCtx(serverAddr());

  folly::CPUThreadPoolExecutor exec(8);
  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    RDMAService<> client;
    for (size_t i = 0; i < 3; i++) {
      serverDropConnections();
      co_await folly::coro::sleep(std::chrono::milliseconds(200));
      co_await runClient(client, ctx, 512 << 10, 8, false, false, 500_ms, opts);
      serverDropConnections();
      co_await folly::coro::sleep(std::chrono::milliseconds(200));
      co_await startMultiClient(&exec, 128, client, ctx, 512 << 10, 8, false, false, 500_ms, opts);
    }
  }));
}

TEST_F(TestRDMA, ServerRestart) {
  auto opts = net::UserRequestOptions{};
  opts.sendRetryTimes = 0;
  auto ctx = client_.serdeCtx(serverAddr());

  folly::CPUThreadPoolExecutor exec(8);
  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    RDMAService<> client;
    for (size_t i = 0; i < 5; i++) {
      serverRestart();
      co_await runClient(client, ctx, 512 << 10, 8, false, false, 500_ms, opts);
      serverRestart();
      co_await startMultiClient(&exec, 128, client, ctx, 512 << 10, 8, false, false, 500_ms, opts);
    }
  }));
}

}  // namespace hf3fs::net