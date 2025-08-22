#include <atomic>
#include <chrono>
#include <folly/Random.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <gtest/gtest.h>
#include <map>
#include <thread>
#include <vector>

#include "SetupIB.h"
#include "common/net/ib/IBDevice.h"
#include "common/net/ib/RDMABuf.h"
#include "common/utils/Coroutine.h"
#include "gtest/gtest.h"

namespace hf3fs::net {

class TestRDMARemoteBuf : public test::SetupIB {};

TEST_F(TestRDMARemoteBuf, Basic) {
  RDMARemoteBuf buf;
  ASSERT_FALSE(buf);
  for (auto rkey : buf.rkeys_) {
    EXPECT_EQ(rkey, RDMARemoteBuf::Rkey());
  }

  auto rdmaBuf = RDMABuf::allocate(4 << 20);
  ASSERT_TRUE(rdmaBuf);
  buf = rdmaBuf.toRemoteBuf();
  ASSERT_TRUE(buf);
}

TEST_F(TestRDMARemoteBuf, Subrange) {
  auto rdmaBuf = RDMABuf::allocate(4 << 20);
  ASSERT_TRUE(rdmaBuf);
  const auto buf = rdmaBuf.toRemoteBuf();
  ASSERT_TRUE(buf);
  ASSERT_EQ(buf.size(), 4 << 20);

  auto buf1 = buf;
  ASSERT_EQ(buf1, buf);
  ASSERT_EQ(buf1.advance(1 << 20), true);
  ASSERT_EQ(buf1.size(), 3 << 20);
  ASSERT_EQ(buf1.subtract(1 << 20), true);
  ASSERT_EQ(buf1.size(), 2 << 20);
  ASSERT_FALSE(buf1.advance(buf1.size() + 1));
  ASSERT_FALSE(buf1.subtract(buf1.size() + 1));

  ASSERT_NE(buf, buf1);
  ASSERT_EQ(buf.subrange(1 << 20, 2 << 20), buf1);
  ASSERT_EQ(buf.subrange(2 << 20, 1 << 20), buf1.subrange(1 << 20, 1 << 20));

  auto buf2 = buf;
  auto buf3 = buf2.first(1 << 20);
  auto buf4 = buf2.takeFirst(1 << 20);
  ASSERT_EQ(buf3, buf4);
  ASSERT_EQ(buf2, buf.subrange(1 << 20, 3 << 20));
  ASSERT_EQ(buf3.addr(), buf.addr());
  ASSERT_EQ(buf2.size(), 3 << 20);
  ASSERT_EQ(buf2.addr(), buf.addr() + (1 << 20));

  auto buf5 = buf;
  auto buf6 = buf.last(1 << 20);
  auto buf7 = buf5.takeLast(1 << 20);
  ASSERT_EQ(buf6, buf7);
  ASSERT_EQ(buf6, buf.subrange(3 << 20, 1 << 20));
  ASSERT_EQ(buf5.size(), 3 << 20);
  ASSERT_EQ(buf5.addr(), buf.addr());

  ASSERT_TRUE(buf.subrange(1 << 20, 3 << 20));
  ASSERT_FALSE(buf.subrange(4 << 20, 1));
  ASSERT_TRUE(buf.subrange(4 << 20, 0));
  ASSERT_TRUE(buf.first(buf.size()));
  ASSERT_TRUE(buf.last(buf.size()));
  ASSERT_FALSE(buf.first(buf.size() + 1));
  ASSERT_FALSE(buf.last(buf.size() + 1));

  auto buf8 = buf;
  ASSERT_FALSE(buf8.advance(buf.size() + 1));
  ASSERT_FALSE(buf8.subtract(buf.size() + 1));
}

class TestRDMABuf : public test::SetupIB {};

TEST_F(TestRDMABuf, Default) {
  RDMABuf buf;
  ASSERT_FALSE(buf);
  ASSERT_FALSE(buf.toRemoteBuf());
  ASSERT_FALSE(buf.toRemoteBuf().getRkey(0));

  RDMABuf buf2 = buf;
  ASSERT_FALSE(buf2);
}

TEST_F(TestRDMABuf, Allocate) {
  std::queue<RDMABuf> bufs;
  for (auto i = 0; i < 100; i++) {
    auto size = folly::Random::rand32(8, 8 << 20);
    auto buf = RDMABuf::allocate(size);
    ASSERT_TRUE(buf);
    ASSERT_NE(buf.ptr(), nullptr);
    ASSERT_EQ(buf.size(), size);
    bufs.push(buf);
  }

  while (!bufs.empty()) {
    auto buf = std::move(bufs.front());
    bufs.pop();

    std::map<int, RDMABufMR> map;
    for (auto &dev : IBDevice::all()) {
      auto mr = buf.getMR(dev->id());
      map[dev->id()] = mr;
      ASSERT_NE(mr, nullptr);
    }
    ASSERT_TRUE(buf.toRemoteBuf());
    // ASSERT_EQ(buf.reregMR(), 0);
    // for (auto &dev : IBDevice::all()) {
    //   auto mr = buf.getMR(dev->id());
    //   ASSERT_NE(mr, nullptr);
    //   ASSERT_NE(mr, map[dev->id()]);
    // }
  }
}

TEST_F(TestRDMABuf, Subrange) {
  const auto buf = RDMABuf::allocate(4 << 20);
  ASSERT_TRUE(buf);
  ASSERT_EQ(buf.size(), 4 << 20);
  ASSERT_EQ(buf.capacity(), buf.size());

  auto buf1 = buf;
  ASSERT_EQ(buf1, buf);
  ASSERT_EQ(buf1.advance(1 << 20), true);
  ASSERT_EQ(buf1.capacity(), 4 << 20);
  ASSERT_EQ(buf1.size(), 3 << 20);
  ASSERT_EQ(buf1.subtract(1 << 20), true);
  ASSERT_EQ(buf1.capacity(), 4 << 20);
  ASSERT_EQ(buf1.size(), 2 << 20);
  ASSERT_FALSE(buf1.advance(buf1.size() + 1));
  ASSERT_FALSE(buf1.subtract(buf1.size() + 1));

  ASSERT_NE(buf, buf1);
  ASSERT_EQ(buf.subrange(1 << 20, 2 << 20), buf1);
  ASSERT_EQ(buf.subrange(2 << 20, 1 << 20), buf1.subrange(1 << 20, 1 << 20));

  auto buf2 = buf;
  auto buf3 = buf2.first(1 << 20);
  auto buf4 = buf2.takeFirst(1 << 20);
  ASSERT_EQ(buf3, buf4);
  ASSERT_EQ(buf2, buf.subrange(1 << 20, 3 << 20));
  ASSERT_EQ(buf3.ptr(), buf.ptr());
  ASSERT_EQ(buf2.size(), 3 << 20);
  ASSERT_EQ(buf2.ptr(), buf.ptr() + (1 << 20));

  auto buf5 = buf;
  auto buf6 = buf.last(1 << 20);
  auto buf7 = buf5.takeLast(1 << 20);
  ASSERT_EQ(buf6, buf7);
  ASSERT_EQ(buf6, buf.subrange(3 << 20, 1 << 20));
  ASSERT_EQ(buf5.size(), 3 << 20);
  ASSERT_EQ(buf5.ptr(), buf.ptr());

  ASSERT_TRUE(buf.subrange(1 << 20, 3 << 20));
  ASSERT_FALSE(buf.subrange(4 << 20, 1));
  ASSERT_TRUE(buf.subrange(4 << 20, 0));
  ASSERT_TRUE(buf.first(buf.size()));
  ASSERT_TRUE(buf.last(buf.size()));
  ASSERT_FALSE(buf.first(buf.size() + 1));
  ASSERT_FALSE(buf.last(buf.size() + 1));

  auto buf8 = buf;
  ASSERT_FALSE(buf8.advance(buf.size() + 1));
  ASSERT_FALSE(buf8.subtract(buf.size() + 1));
}

class TestRDMABufPool : public test::SetupIB {};

TEST_F(TestRDMABufPool, Basic) {
  constexpr size_t kBufSize = 4 << 10L;
  constexpr size_t kBufNum = 10;
  auto pool = RDMABufPool::create(kBufSize, kBufNum);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    std::queue<RDMABuf> bufs;
    for (size_t i = 0; i < kBufNum; i++) {
      auto buf = co_await pool->allocate();
      CO_ASSERT_TRUE(buf);
      bufs.push(buf);
    }

    auto buf = co_await pool->allocate(std::chrono::milliseconds(10));
    CO_ASSERT_FALSE(buf);

    auto firstPtr = bufs.front().ptr();
    std::atomic_bool allocated(false);
    auto firstBuf = bufs.front();
    bufs.pop();
    std::jthread proc([&allocated, first = std::move(firstBuf)]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      EXPECT_EQ(allocated.load(), false);
    });
    buf = co_await pool->allocate();
    CO_ASSERT_EQ(firstPtr, buf.ptr());
    bufs.push(buf);

    while (!bufs.empty()) {
      auto buf = bufs.front();
      bufs.pop();
      CO_ASSERT_EQ(buf.capacity(), kBufSize);
      CO_ASSERT_NE(buf.ptr(), nullptr);
      std::map<int, RDMABufMR> map;
      for (auto &dev : IBDevice::all()) {
        auto mr = buf.getMR(dev->id());
        map[dev->id()] = mr;
        CO_ASSERT_NE(mr, nullptr);
      }
      buf.toRemoteBuf();
      // CO_ASSERT_EQ(buf.reregMR(), 0);
      // for (auto &dev : IBDevice::all()) {
      //   auto mr = buf.getMR(dev->id());
      //   CO_ASSERT_NE(mr, nullptr);
      //   CO_ASSERT_NE(mr, map[dev->id()]);
      // }
    }
  }());
}

}  // namespace hf3fs::net
