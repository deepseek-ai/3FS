#include <algorithm>
#include <atomic>
#include <bits/types/struct_iovec.h>
#include <boost/thread/barrier.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fmt/core.h>
#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/Random.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/SocketAddress.h>
#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/experimental/coro/Baton.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/futures/detail/Types.h>
#include <folly/hash/Checksum.h>
#include <folly/io/async/AsyncServerSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/coro/ServerSocket.h>
#include <folly/io/coro/Transport.h>
#include <folly/logging/xlog.h>
#include <folly/net/NetworkSocket.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <infiniband/verbs.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <queue>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

#include "SetupIB.h"
#include "common/net/Client.h"
#include "common/net/MessageHeader.h"
#include "common/net/Network.h"
#include "common/net/Server.h"
#include "common/net/WriteItem.h"
#include "common/net/ib/IBConnect.h"
#include "common/net/ib/IBConnectService.h"
#include "common/net/ib/IBDevice.h"
#include "common/net/ib/IBSocket.h"
#include "common/net/ib/RDMABuf.h"
#include "common/serde/ClientContext.h"
#include "common/utils/Address.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::net::test {

// DEFINE_uint32(gid_index, 0, "RoCE gid index.");
DEFINE_bool(ib_wait_fd, true, "IBSocket wait on fd instead of busy polling.");
DEFINE_uint64(ib_bench_mb, (4 << 10), "IBSocket benchmark send data size (mb).");
DEFINE_uint32(ib_bench_buf_kb, 0, "IBSocket benchmark send buf size");
DEFINE_uint32(ib_bench_buf_cnt, 0, "IBSocket benchmark send buf cnt");
DEFINE_uint64(ib_pingpong_loops, 10000, "IBSocket pingpong loops.");
DEFINE_uint64(ib_pingpong_size, 64, "IBSocket pingpong msg size (bytes).");
DEFINE_uint64(ib_rdma_iters, 500, "IBSocket RDMA test iters.");

struct TestUtils {
  static IBSocket::Config testCfg() {
    IBSocket::Config cfg;
    cfg.set_retry_cnt(0);
    cfg.set_buf_size(512);
    cfg.set_send_buf_cnt(32);
    cfg.set_buf_ack_batch(4);
    cfg.set_buf_signal_batch(4);
    cfg.set_max_rdma_wr(7);
    cfg.set_max_rdma_wr_per_post(5);
    cfg.set_max_sge(3);
    return cfg;
  }

  static std::vector<std::jthread> poll(std::vector<IBSocket *> sockets, std::atomic<bool> &stop) {
    std::vector<std::jthread> threads;
    threads.reserve(sockets.size());
    for (auto socket : sockets) {
      threads.emplace_back([&stop, socket]() {
        while (!stop) {
          TestUtils::wait(*socket, 0, 100ms);
        }
      });
    }
    return threads;
  }

  static Result<Socket::Events> wait(IBSocket &socket, Socket::Events events, std::chrono::milliseconds timeout) {
    while (true) {
      if (FLAGS_ib_wait_fd && timeout.count() != 0) {
        pollfd fd{
            .fd = socket.fd(),
            .events = POLL_IN,
            .revents = 0,
        };
        int ret = ::poll(&fd, 1, timeout.count());
        if (ret < 0 || (ret == 0 && timeout.count())) {
          return makeError(RPCCode::kTimeout, fmt::format("poll ret is {}", ret));
        }
      }

      auto result = socket.poll(0);
      if (result.hasError()) {
        return makeError(std::move(result.error()));
      }
      if (events == 0 || (result.value() & events) != 0) {
        return result.value();
      }
    }
  }

  static Result<Void> send(IBSocket &socket, folly::ByteRange buf, bool poll = true) {
    while (!buf.empty()) {
      // try send first
      auto sendResult = socket.send(buf);
      RETURN_ON_ERROR(sendResult);
      buf.advance(sendResult.value());

      if (buf.empty()) {
        break;
      }

      // wait writable
      if (poll) {
        auto waitResult = wait(socket, Socket::kEventWritableFlag, 1000ms);
        RETURN_ON_ERROR(waitResult);
      }
    }

    return socket.flush();
  }

  static Result<Void> send(IBSocket &socket, iovec *iovs, int iovCnts, bool poll) {
    for (int i = 0; i < iovCnts; i++) {
      auto result = send(socket, folly::ByteRange((uint8_t *)iovs[i].iov_base, iovs[i].iov_len), poll);
      RETURN_ON_ERROR(result);
    }

    return Void{};
  }

  static Result<Void> recv(IBSocket &socket, folly::MutableByteRange buf, bool poll = true) {
    if (buf.empty()) {
      return Void{};
    }
    while (true) {
      auto recvResult = socket.recv(buf);
      RETURN_ON_ERROR(recvResult);
      buf.advance(recvResult.value());

      if (buf.empty()) {
        break;
      }
      // wait readable
      if (poll) {
        auto waitResult = wait(socket, Socket::kEventReadableFlag, 1000ms);
        RETURN_ON_ERROR(waitResult);
      }
    }

    return Void{};
  }
};

class TestMsg {
 public:
  static Result<Void> send(IBSocket &socket, size_t bytes, bool poll = true) {
    TestMsg msg;
    msg.bytes_ = bytes;
    msg.data_.resize(bytes);
    for (size_t i = 0; i < bytes; i++) {
      msg.data_[i] = (uint8_t)folly::Random::rand32();
    }
    msg.crc32c_ = folly::crc32c(msg.data_.data(), msg.data_.size(), 0);

    iovec iovs[] = {
        {.iov_base = &msg.bytes_, .iov_len = sizeof(msg.bytes_)},
        {.iov_base = &msg.crc32c_, .iov_len = sizeof(msg.crc32c_)},
        {.iov_base = msg.data_.data(), .iov_len = msg.data_.size()},
    };

    return TestUtils::send(socket, iovs, sizeof(iovs) / sizeof(iovs[0]), poll);
  }

  static Result<Void> recv(IBSocket &socket, bool poll = true) {
    TestMsg msg;
    auto result =
        TestUtils::recv(socket,
                        folly::MutableByteRange((uint8_t *)&msg.bytes_, sizeof(msg.bytes_) + sizeof(msg.crc32c_)),
                        poll);
    RETURN_ON_ERROR(result);
    msg.data_.resize(msg.bytes_);
    result = TestUtils::recv(socket, folly::MutableByteRange(msg.data_.data(), msg.data_.size()), poll);
    RETURN_ON_ERROR(result);

    uint32_t crc32c = folly::crc32c(msg.data_.data(), msg.data_.size(), 0);
    EXPECT_EQ(msg.crc32c_, crc32c);
    if (msg.crc32c_ != crc32c) {
      return makeError(StatusCode::kDataCorruption);
    }
    return Void{};
  }

 private:
  uint32_t bytes_ = 0;
  uint32_t crc32c_ = 0;
  std::vector<uint8_t> data_;
};

class TestIBSocket : public SetupIB {
 public:
  auto &group() { return *server_.groups().front(); }
  const auto &group() const { return *server_.groups().front(); }
  Address serverAddr() const { return group().addressList().front(); }

  void SetUp() override {
    ASSERT_TRUE(server_.setup());
    auto accept = [&](auto socket) { accepted_.lock()->push(std::move(socket)); };
    auto service = std::make_unique<IBConnectService>(serverConfig_.groups(0).io_worker().ibsocket(), accept, []() {
      return 5_s;
    });
    group().addSerdeService(std::move(service), Address::Type::TCP);
    ASSERT_TRUE(server_.start());
    client_.start();
  }

  void TearDown() override {
    client_.stopAndJoin();
    server_.stopAndJoin();
  }

  CoTryTask<std::pair<std::unique_ptr<IBSocket>, std::unique_ptr<IBSocket>>> connect(
      IBSocket::Config &cfg,
      std::chrono::milliseconds timeout = 220ms) {
    // cfg.set_gid_index(FLAGS_gid_index);
    auto addr = serverAddr();
    Address tcpAddr(addr.ip, addr.port, Address::TCP);
    auto ctx = client_.serdeCtx(tcpAddr);

    auto clientSocket = std::make_unique<IBSocket>(cfg);
    auto result = co_await clientSocket->connect(ctx, Duration{timeout});
    CO_RETURN_ON_ERROR(result);

    auto queue = accepted_.lock();
    EXPECT_EQ(queue->size(), 1);
    auto serverSocket = std::move(queue->front());
    queue->pop();
    queue.unlock();

    EXPECT_FALSE(serverSocket->check().hasError());
    EXPECT_FALSE(clientSocket->check().hasError());

    auto serverReady = TestUtils::wait(*serverSocket, Socket::kEventWritableFlag, 500ms);
    auto clientReady = TestUtils::wait(*clientSocket, Socket::kEventWritableFlag, 500ms);
    EXPECT_FALSE(serverReady.hasError()) << serverReady.error().describe();
    EXPECT_FALSE(clientReady.hasError()) << serverReady.error().describe();
    CO_RETURN_ON_ERROR(serverReady);
    CO_RETURN_ON_ERROR(clientReady);

    co_return std::pair(std::move(serverSocket), std::move(clientSocket));
  }

  Result<std::pair<std::unique_ptr<IBSocket>, std::unique_ptr<IBSocket>>> connectSync(
      IBSocket::Config &cfg,
      std::chrono::milliseconds timeout = 100ms) {
    return folly::coro::blockingWait(connect(cfg, timeout));
  }

 protected:
  Client::Config clientConfig_;
  Client client_{clientConfig_};

  Server::Config serverConfig_;
  bool initServerConfig = [this] {
    serverConfig_.groups(0).set_network_type(Address::TCP);
    serverConfig_.groups(0).listener().set_reuse_port(true);
    return true;
  }();
  Server server_{serverConfig_};
  folly::Synchronized<std::queue<std::unique_ptr<IBSocket>>, std::mutex> accepted_;
};

TEST_F(TestIBSocket, InvalidConfig) {
  auto dev = IBDevice::get(0);
  ASSERT_NE(dev, nullptr);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    IBSocket::Config cfg;
    cfg.set_max_rd_atomic(dev->attr().max_qp_rd_atom + 1);

    auto result = co_await connect(cfg);
    CO_ASSERT_ERROR(result, StatusCode::kInvalidConfig);
  }());
}

TEST_F(TestIBSocket, ConnectTimeout) {
  server_.stopAndJoin();

  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto config = TestUtils::testCfg();
    auto result = co_await connect(config);
    CO_ASSERT_ERROR(result, RPCCode::kSendFailed);
  }());
}

TEST_F(TestIBSocket, Connect) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto config = TestUtils::testCfg();
    auto result = co_await connect(config);
    CO_ASSERT_OK(result);
    IBManager::close(std::move(result->first));
    IBManager::close(std::move(result->second));
  }());
}

TEST_F(TestIBSocket, ClientToServer) {
  static constexpr int msgs = 100;

  auto config = TestUtils::testCfg();
  auto result = connectSync(config);
  ASSERT_OK(result);

  std::jthread server([socket = std::move(result->first)]() {
    while (true) {
      auto msg = TestMsg::recv(*socket);
      if (msg.hasError()) {
        ASSERT_ERROR(msg, RPCCode::kSocketClosed);
        break;
      }
    }
  });
  std::jthread client([socket = std::move(result->second)]() mutable {
    for (int i = 0; i < msgs; i++) {
      ASSERT_OK(TestMsg::send(*socket, folly::Random::rand32(4, 2 << 20)));
    }
    IBManager::close(std::move(socket));
  });
}

TEST_F(TestIBSocket, ServerToClient) {
  static constexpr int msgs = 100;

  auto config = TestUtils::testCfg();
  auto result = connectSync(config);
  ASSERT_OK(result);

  std::jthread server([socket = std::move(result->first)]() mutable {
    for (int i = 0; i < msgs; i++) {
      ASSERT_OK(TestMsg::send(*socket, folly::Random::rand32(4, 2 << 20)));
    }
    IBManager::close(std::move(socket));
  });
  std::jthread clntTh([socket = std::move(result->second)]() {
    for ([[maybe_unused]] int i = 0; true; i++) {
      auto msg = TestMsg::recv(*socket);
      if (msg.hasError()) {
        ASSERT_ERROR(msg, RPCCode::kSocketClosed);
        break;
      }
    }
  });
}

TEST_F(TestIBSocket, BIDirection) {
  static constexpr int msgs = 100;

  auto config = TestUtils::testCfg();
  auto result = connectSync(config);
  ASSERT_OK(result);
  SCOPE_EXIT {
    IBManager::close(std::move(result->first));
    IBManager::close(std::move(result->second));
  };

  std::atomic<uint32_t> cnt(0);
  auto send = [&](IBSocket &socket) {
    SCOPE_EXIT { cnt++; };
    for (int i = 0; i < msgs; i++) {
      ASSERT_OK(TestMsg::send(socket, folly::Random::rand32(4, 2 << 20), false));
    }
  };
  auto recv = [&](IBSocket &socket) {
    SCOPE_EXIT { cnt++; };
    for (int i = 0; i < msgs; i++) {
      ASSERT_OK(TestMsg::recv(socket, false));
    }
  };
  auto poll = [&](IBSocket &socket) {
    while (cnt < 4) {
      auto result = socket.poll(0);
      if (result.hasError()) {
        ASSERT_ERROR(result, RPCCode::kSocketClosed);
        break;
      }
    }
  };

  std::jthread server_send([&]() { send(*result->first); });
  std::jthread server_recv([&]() { recv(*result->first); });
  std::jthread server_poll([&]() { poll(*result->first); });
  std::jthread client_send([&]() { send(*result->second); });
  std::jthread client_recv([&]() { recv(*result->second); });
  std::jthread client_poll([&]() { poll(*result->second); });
}

TEST_F(TestIBSocket, BIDirectionFaultInjection) {
  auto config = TestUtils::testCfg();
  auto result = connectSync(config);
  ASSERT_OK(result);
  SCOPE_EXIT {
    IBManager::close(std::move(result->first));
    IBManager::close(std::move(result->second));
  };

  std::atomic<uint32_t> cnt(0);

  auto inject = [&](IBSocket &socket) {
    SCOPE_EXIT { cnt++; };
    folly::coro::blockingWait([&]() -> CoTask<void> {
      // try to inject a error by post a invalid RDMA operation, shouldn't receive corruption message.
      co_await folly::coro::sleep(std::chrono::milliseconds(1000));
      // underlying RDMABuf is deregistered immediately, so remote buffer is invalid
      auto remote = RDMABuf::allocate(1024).toRemoteBuf();
      auto local = RDMABuf::allocate(1024);
      auto batch = socket.rdmaWriteBatch();
      batch.add(remote, local);
      auto result = co_await batch.post();
      CO_ASSERT_TRUE(result.hasError());
    }());
  };
  auto send = [&](IBSocket &socket) {
    SCOPE_EXIT { cnt++; };
    while (true) {
      auto result = TestMsg::send(socket, folly::Random::rand32(4, 2 << 20), false);
      if (result.hasError()) {
        ASSERT_ERROR(result, RPCCode::kSocketError);
        break;
      }
    }
  };
  auto recv = [&](IBSocket &socket) {
    SCOPE_EXIT { cnt++; };
    while (true) {
      auto result = TestMsg::recv(socket, false);
      if (result.hasError()) {
        ASSERT_ERROR(result, RPCCode::kSocketError);
        break;
      }
    }
  };
  auto poll = [&](IBSocket &socket) {
    while (cnt < 5) {
      auto result = socket.poll(0);
      if (result.hasError()) {
        bool closed = result.error().code() == RPCCode::kSocketClosed;
        bool error = result.error().code() == RPCCode::kSocketError;
        ASSERT_TRUE(closed || error) << result.error().describe();
        break;
      }
    }
  };

  std::jthread inject_th([&]() { inject(*result->first); });
  std::jthread server_send([&]() { send(*result->first); });
  std::jthread server_recv([&]() { recv(*result->first); });
  std::jthread server_poll([&]() { poll(*result->first); });
  std::jthread client_send([&]() { send(*result->second); });
  std::jthread client_recv([&]() { recv(*result->second); });
  std::jthread client_poll([&]() { poll(*result->second); });
}

TEST_F(TestIBSocket, Benchmark) {
  size_t benchSize = FLAGS_ib_bench_mb * (1ULL << 20);
  auto config = IBSocket::Config();
  if (FLAGS_ib_bench_buf_kb) config.set_buf_size(FLAGS_ib_bench_buf_kb * 1024);
  if (FLAGS_ib_bench_buf_cnt) config.set_send_buf_cnt(FLAGS_ib_bench_buf_cnt);
  auto result = connectSync(config);
  ASSERT_OK(result);

  UtcTime begin = UtcClock::now();
  std::jthread server([benchSize, socket = std::move(result->first)]() {
    std::vector<uint8_t> buf(4 << 20, 0);
    size_t recved = 0;
    while (recved < benchSize) {
      size_t rsize = std::min(buf.size(), benchSize - recved);
      auto result = TestUtils::recv(*socket, folly::MutableByteRange(buf.data(), rsize));
      ASSERT_OK(result);
      recved += rsize;
    }
  });
  std::jthread client([benchSize, socket = std::move(result->second)]() {
    std::vector<uint8_t> buf(4 << 20, 0);
    size_t sended = 0;
    while (sended < benchSize) {
      size_t wsize = std::min(buf.size(), benchSize - sended);
      auto result = TestUtils::send(*socket, folly::ByteRange(buf.data(), wsize));
      ASSERT_OK(result);
      sended += wsize;
    }
  });
  server.join();
  client.join();

  auto dur = UtcClock::now() - begin;
  std::cout << "IBSocket benchmark bandwidth: "
            << ((double)FLAGS_ib_bench_mb / std::chrono::duration_cast<std::chrono::milliseconds>(dur).count() * 1000.0)
            << " MB/s" << std::endl;
}

TEST_F(TestIBSocket, PingPong) {
  size_t kLoops = FLAGS_ib_pingpong_loops;
  size_t kMsgSize = FLAGS_ib_pingpong_size;

  auto config = IBSocket::Config();
  auto result = connectSync(config);
  ASSERT_OK(result);
  UtcTime begin = UtcClock::now();
  std::jthread server([kMsgSize, kLoops, socket = std::move(result->first)]() {
    std::vector<uint8_t> buf(kMsgSize, 0);
    for (size_t i = 0; i < kLoops; i++) {
      ASSERT_OK(TestUtils::recv(*socket, folly::MutableByteRange(buf.data(), kMsgSize)));
      ASSERT_OK(TestUtils::send(*socket, folly::ByteRange(buf.data(), kMsgSize)));
    }
  });
  std::jthread client([kMsgSize, kLoops, socket = std::move(result->second)]() {
    std::vector<uint8_t> buf(kMsgSize, 0);
    for (size_t i = 0; i < kLoops; i++) {
      ASSERT_OK(TestUtils::send(*socket, folly::ByteRange(buf.data(), kMsgSize)));
      ASSERT_OK(TestUtils::recv(*socket, folly::MutableByteRange(buf.data(), kMsgSize)));
    }
  });
  server.join();
  client.join();

  auto dur = UtcClock::now() - begin;
  std::cout << "IBSocket pingpong latency: "
            << std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count() / 1000.0 / kLoops << "us" << std::endl;
}

TEST_F(TestIBSocket, RDMAWrite) {
  auto config = TestUtils::testCfg();
  auto result = connectSync(config);
  ASSERT_OK(result);
  SCOPE_EXIT {
    IBManager::close(std::move(result->first));
    IBManager::close(std::move(result->second));
  };

  std::atomic<bool> stop = false;
  auto jthreads = TestUtils::poll({result->first.get(), result->second.get()}, stop);

  folly::coro::blockingWait([&, &socket = *result->first]() -> CoTask<void> {
    auto pool = RDMABufPool::create(4 << 10, 10);
    for (size_t i = 0; i < FLAGS_ib_rdma_iters; i++) {
      XLOGF_IF(INFO, i % 100 == 0, "Run {}", i);
      // RDMABufPool
      auto src = co_await pool->allocate();
      auto dst = co_await pool->allocate();
      CO_ASSERT_TRUE(src);
      CO_ASSERT_TRUE(dst);

      for (size_t i = 0; i < src.size(); i++) {
        ((uint8_t *)src.ptr())[i] = folly::Random::rand32();
      }
      auto crc32c = folly::crc32c((uint8_t *)src.ptr(), src.size());

      auto dstRemote = dst.toRemoteBuf();
      CO_ASSERT_TRUE(dstRemote);

      auto result = co_await socket.rdmaWrite(dstRemote, src);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(folly::crc32c((uint8_t *)dst.ptr(), dst.size()), crc32c);
    }

    auto remotePool = RDMABufPool::create(4096 * 16, -1);
    auto localPool = RDMABufPool::create(4096, -1);
    for (size_t i = 0; i < FLAGS_ib_rdma_iters; i++) {
      XLOGF_IF(INFO, i % 100 == 0, "Run {}", i);
      std::vector<RDMABuf> remotes;
      std::vector<RDMABuf> locals;
      auto batch = socket.rdmaWriteBatch();
      int numReqs = folly::Random::rand32(1, 64);
      for (int i = 0; i < numReqs; i++) {
        size_t total = 0;
        int numLocalBufs = folly::Random::rand32(1, 16);
        for (int j = 0; j < numLocalBufs; j++) {
          auto size = folly::Random::rand32(4, 4096);
          auto local = co_await localPool->allocate();
          local = local.first(size);
          CO_ASSERT_TRUE(local);
          locals.push_back(local);
          total += size;
        }
        auto remote = co_await remotePool->allocate();
        remote = remote.first(total);
        CO_ASSERT_TRUE(remote);
        remotes.push_back(remote);

        batch.add(remote.toRemoteBuf(), std::span(locals).last(numLocalBufs));
      }

      auto crc32c = 0;
      for (auto buf : locals) {
        for (size_t i = 0; i < buf.size(); i++) {
          ((uint8_t *)buf.ptr())[i] = folly::Random::rand32();
        }
        crc32c = folly::crc32c((uint8_t *)buf.ptr(), buf.size(), crc32c);
      }

      auto result = co_await batch.post();
      CO_ASSERT_OK(result);

      auto crc32cOut = 0;
      for (auto buf : remotes) {
        crc32cOut = folly::crc32c((uint8_t *)buf.ptr(), buf.size(), crc32cOut);
      }
      CO_ASSERT_EQ(crc32c, crc32cOut);
    }

    stop = true;
  }());
}

TEST_F(TestIBSocket, RDMARead) {
  auto config = TestUtils::testCfg();
  auto result = connectSync(config);
  ASSERT_OK(result);
  SCOPE_EXIT {
    IBManager::close(std::move(result->first));
    IBManager::close(std::move(result->second));
  };

  std::atomic<bool> stop = false;
  auto jthreads = TestUtils::poll({result->first.get(), result->second.get()}, stop);

  folly::coro::blockingWait([&, &socket = *result->first]() -> CoTask<void> {
    auto pool = RDMABufPool::create(4 << 10, 10);
    for (size_t i = 0; i < FLAGS_ib_rdma_iters; i++) {
      XLOGF_IF(INFO, i % 100 == 0, "Run {}", i);
      // RDMABufPool
      auto src = co_await pool->allocate();
      auto dst = co_await pool->allocate();
      CO_ASSERT_TRUE(src);
      CO_ASSERT_TRUE(dst);

      for (size_t i = 0; i < src.size(); i++) {
        ((uint8_t *)src.ptr())[i] = folly::Random::rand32();
      }
      auto crc32c = folly::crc32c((uint8_t *)src.ptr(), src.size());

      auto srcRemote = src.toRemoteBuf();
      CO_ASSERT_TRUE(srcRemote);

      auto result = co_await socket.rdmaRead(srcRemote, dst);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(folly::crc32c((uint8_t *)dst.ptr(), dst.size()), crc32c);
    }

    auto remotePool = RDMABufPool::create(4096 * 16, -1);
    auto localPool = RDMABufPool::create(4096, -1);
    for (size_t i = 0; i < FLAGS_ib_rdma_iters; i++) {
      XLOGF_IF(INFO, i % 100 == 0, "Run {}", i);
      std::vector<RDMABuf> remotes;
      std::vector<RDMABuf> locals;
      auto batch = socket.rdmaReadBatch();
      int numReqs = folly::Random::rand32(1, 64);
      for (int i = 0; i < numReqs; i++) {
        size_t total = 0;
        int numLocalBufs = folly::Random::rand32(1, 16);
        for (int j = 0; j < numLocalBufs; j++) {
          auto size = folly::Random::rand32(4, 4 << 10);
          auto local = co_await localPool->allocate();
          local = local.first(size);
          CO_ASSERT_TRUE(local);
          locals.push_back(local);
          total += size;
        }
        auto remote = co_await remotePool->allocate();
        remote = remote.first(total);
        CO_ASSERT_TRUE(remote);
        remotes.push_back(remote);

        batch.add(remote.toRemoteBuf(), std::span(locals).last(numLocalBufs));
      }

      auto crc32c = 0;
      for (auto buf : remotes) {
        for (size_t i = 0; i < buf.size(); i++) {
          ((uint8_t *)buf.ptr())[i] = folly::Random::rand32();
        }
        crc32c = folly::crc32c((uint8_t *)buf.ptr(), buf.size(), crc32c);
      }

      auto result = co_await batch.post();
      CO_ASSERT_OK(result);

      auto crc32cOut = 0;
      for (auto buf : locals) {
        crc32cOut = folly::crc32c((uint8_t *)buf.ptr(), buf.size(), crc32cOut);
      }
      CO_ASSERT_EQ(crc32c, crc32cOut);
    }

    stop = true;
  }());
}

TEST_F(TestIBSocket, RemoteClosed) {
  auto config = IBSocket::Config();
  auto result = connectSync(config);
  ASSERT_OK(result);

  boost::barrier barrier(2);
  std::jthread server([socket = std::move(result->first)]() mutable {
    ASSERT_OK(TestMsg::send(*socket, 4 << 20));
    IBManager::close(std::move(socket));
  });
  std::jthread client([socket = std::move(result->second)]() mutable {
    ASSERT_OK(TestMsg::recv(*socket));
    ASSERT_ERROR(TestMsg::recv(*socket), RPCCode::kSocketClosed);
    ASSERT_ERROR(TestMsg::send(*socket, 1 << 20), RPCCode::kSocketClosed);
    ASSERT_ERROR(TestMsg::send(*socket, 1 << 20), RPCCode::kSocketClosed);
    IBManager::close(std::move(socket));
  });
}

TEST_F(TestIBSocket, RDMAFailure) {
  auto config = TestUtils::testCfg();
  auto result = connectSync(config);
  ASSERT_OK(result);
  SCOPE_EXIT {
    IBManager::close(std::move(result->first));
    IBManager::close(std::move(result->second));
  };

  // connection is OK
  {
    auto &client = *result->first;
    auto deadline = RelativeTime::now() + 5_s;
    while (RelativeTime::now() < deadline) {
      ASSERT_OK(client.check());
      auto result = client.poll(0);
      ASSERT_OK(result);
    }
  }

  // post invalid RDMA request on server side to make
  std::atomic<bool> stop = false;
  auto jthreads = TestUtils::poll({result->second.get()}, stop);
  folly::coro::blockingWait([&, &server = *result->second]() -> CoTask<void> {
    // post RDMA with invalid rkey
    // underlying RDMABuf is deregistered immediately, so remote buffer is invalid
    auto remote = RDMABuf::allocate(1024).toRemoteBuf();
    auto local = RDMABuf::allocate(1024);
    for (int i = 0; i < 2; i++) {
      auto batch = server.rdmaWriteBatch();
      batch.add(remote, local);
      batch.add(remote, local);
      batch.add(remote, local);
      batch.add(remote, local);
      auto result = co_await batch.post();
      CO_ASSERT_TRUE(result.hasError());
      std::cout << result.error().describe() << std::endl;
    }
    stop = true;
  }());

  // connection is broken, send check message on client side
  auto &client = *result->first;
  auto deadline = RelativeTime::now() + 10_s;
  auto error = false;
  while (RelativeTime::now() < deadline) {
    ASSERT_OK(client.check());
    auto result = client.poll(0);
    if (result.hasError()) {
      error = true;
      fmt::print("{}\n", result.error());
      break;
    } else {
      fmt::print(".");
    }
    std::this_thread::sleep_for(1_s);
  }
  ASSERT_TRUE(error) << "client side doesn't get error after send check message";
}

TEST_F(TestIBSocket, CloseBeforeConnect) {
  auto config = TestUtils::testCfg();
  auto addr = serverAddr();
  Address tcpAddr(addr.ip, addr.port, Address::TCP);
  auto ctx = client_.serdeCtx(tcpAddr);

  auto socket = std::make_unique<IBSocket>(config);
  folly::coro::blockingWait(socket->close());
  ASSERT_ERROR(folly::coro::blockingWait(socket->connect(ctx, 1_s)), RPCCode::kSocketClosed);
}

}  // namespace hf3fs::net::test
