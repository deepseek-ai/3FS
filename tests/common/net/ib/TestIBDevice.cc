#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fmt/compile.h>
#include <fmt/core.h>
#include <folly/Conv.h>
#include <folly/IPAddressV4.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/futures/detail/Types.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <infiniband/verbs.h>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

#include "SetupIB.h"
#include "common/net/IfAddrs.h"
#include "common/net/ib/IBConnect.h"
#include "common/net/ib/IBDevice.h"
#include "common/utils/Duration.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/UtcTime.h"
#include "tests/GtestHelpers.h"

DEFINE_uint64(ibdev_reg_mem_size, 1ull << 30, "Total memory size to register.");
DEFINE_uint32(ibdev_reg_mem_ms, 100, "Time to run reg memory test for each case.");

DEFINE_uint64(reg_gb, 1, "reg memory size.");
DEFINE_bool(reg_odp, false, "odp");
DEFINE_bool(reg_releaxed_ording, false, "releaxed ording");
DEFINE_uint32(reg_split, 1, "reg split large memory");

namespace hf3fs::net {

class TestIBDevice : public test::SetupIB {};

TEST_F(TestIBDevice, IfAddrs) {
  auto result = IfAddrs::load();
  ASSERT_TRUE(!result.hasError());
  for (auto [name, addr] : *result) {
    fmt::print("{} -> ip {}, mask {}, up {}\n", name, addr.ip.str(), addr.mask, addr.up);
  }
}

TEST_F(TestIBDevice, Normal) {
  for (auto &dev : IBDevice::all()) {
    fmt::print("dev: {}\n", dev->name());
    ASSERT_NE(dev->ports().size(), 0);
    ASSERT_NE(dev->context(), nullptr);
    for (size_t portNum = 1; portNum <= dev->ports().size(); portNum++) {
      auto port = dev->openPort(portNum);
      ASSERT_FALSE(port.hasError()) << port.error().describe();
      fmt::print("\tport {}, zones {}\n", *port, fmt::join(port->zones().begin(), port->zones().end(), ";"));
      ASSERT_EQ(port->dev(), dev.get());

      if (port->isRoCE()) {
        auto r = port->getRoCEv2Gid();
        fmt::print("\tRoCE v2 GID[{}]: {}\n", r.second, r.first);
      }

      for (int i = 0; true; i++) {
        ibv_gid gid;
        auto ret = ibv_query_gid(dev->context(), port->portNum(), i, &gid);
        if (ret || std::all_of(&gid.raw[0], &gid.raw[sizeof(gid)], [](auto v) { return v == 0; })) {
          break;
        }
        auto ip = folly::IPAddressV6::fromBinary(folly::ByteRange(gid.raw, sizeof(gid.raw)));
        fmt::print("\t\tGID[{}]: {}, {} {}\n", i, gid, ip.str(), ip.is6To4());
      }

      for (int i = 0; i < 1000; i++) {
        __be16 pkey;
        auto ret = ibv_query_pkey(dev->context(), portNum, i, &pkey);
        if (ret != 0 || pkey == 0) {
          break;
        }
        fmt::print("dev {}, port {}, pkey {} => {:x}\n", dev->name(), portNum, i, pkey);
      }
    }
  }
}

TEST_F(TestIBDevice, UpdatePortStatus) {
  ASSERT_FALSE(IBDevice::all().empty());
  ASSERT_FALSE(IBDevice::get(0)->ports().empty());

  for (auto &dev : IBDevice::all()) {
    for (auto &[portNum, port] : dev->ports()) {
      auto state = port.attr.rlock()->state;

      // disable a port by set state to DOWN
      port.attr.withWLock([](auto &attr) { attr.state = IBV_PORT_DOWN; });
      // open port, then port state should be updated
      auto result = dev->openPort(portNum);
      ASSERT_OK(result);
      ASSERT_EQ(result->attr().state, state);
      ASSERT_EQ(port.attr.rlock()->state, state);

      // disable a port by set state to DOWN
      port.attr.withWLock([](auto &attr) { attr.state = IBV_PORT_DOWN; });
      // wait sometime, period runner will update port status
      auto begin = SteadyClock::now();
      while (true) {
        auto attr = *port.attr.rlock();
        if (attr.state == state) {
          break;
        }
        ASSERT_TRUE(SteadyClock::now() < begin + 30_s) << "port status doesn't update in 30s";
        std::this_thread::sleep_for(1_s);
      }
    }
  }
}

auto regFlags() {
  auto flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
  if (FLAGS_reg_odp) {
    flags |= IBV_ACCESS_ON_DEMAND;
  }
  if (FLAGS_reg_releaxed_ording) {
    flags |= IBV_ACCESS_RELAXED_ORDERING;
  }
  return flags;
}

TEST_F(TestIBDevice, RegMemory) {
  void *mem = malloc(FLAGS_ibdev_reg_mem_size);
  auto guard = folly::makeGuard([&]() { free(mem); });
  auto benchmark = [&]() {
    for (uint64_t bs = 4096; bs <= (16 << 20); bs *= 4) {
      auto numBlock = FLAGS_ibdev_reg_mem_size / bs;
      std::chrono::nanoseconds regDur(0), deregDur(0);
      auto begin = std::chrono::steady_clock::now();
      size_t iters = 0;
      while (std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(FLAGS_ibdev_reg_mem_ms)) {
        std::array<ibv_mr *, 100> mrs;
        auto regBegin = std::chrono::steady_clock::now();
        for (size_t i = 0; i < mrs.size(); i++) {
          size_t offset = (i % numBlock) * bs;
          void *ptr = (uint8_t *)mem + offset;
          mrs[i] = IBDevice::get(0)->regMemory(ptr, bs, regFlags());

          iters++;
        }
        regDur += std::chrono::steady_clock::now() - regBegin;

        auto deregBegin = std::chrono::steady_clock::now();
        for (auto mr : mrs) {
          IBDevice::get(0)->deregMemory(mr);
        }
        deregDur += std::chrono::steady_clock::now() - deregBegin;
      }

      fmt::print("bs: {:8}, reg {:192.168f} us, dereg {:192.168f} us\n",
                 bs,
                 regDur.count() / 1000.0 / iters,
                 deregDur.count() / 1000.0 / iters);
    }
  };

  benchmark();
  auto mr = IBDevice::get(0)->regMemory(mem, FLAGS_ibdev_reg_mem_size, regFlags());
  fmt::print("Register reged memory.\n");
  benchmark();
  IBDevice::get(0)->deregMemory(mr);
}

TEST_F(TestIBDevice, DISABLED_RegMR) {
  std::atomic_bool exit = false;
  auto progress = std::jthread([&]() {
    while (!exit) {
      sleep(5);
      XLOGF(INFO, "still alive");
    }
  });

  auto memsize = FLAGS_reg_gb * (1ULL << 30);
  XLOGF(INFO, "malloc begin {}", memsize);
  auto *mem = (uint8_t *)malloc(memsize);
  XLOGF(INFO, "malloc success, {} {}", memsize, (void *)mem);

  std::vector<ibv_mr *> mrs;
  auto bs = memsize / FLAGS_reg_split;
  XLOGF(INFO, "begin reg");
  for (size_t i = 0; i < FLAGS_reg_split; i++) {
    auto mr = IBDevice::get(0)->regMemory(mem + bs * i, bs, regFlags());
    XLOGF(INFO, "reg get {}", (void *)mr);
    ASSERT_NE(mr, nullptr) << memsize;
    mrs.push_back(mr);
  }

  XLOGF(INFO, "dereg begin");
  for (auto mr : mrs) {
    IBDevice::get(0)->deregMemory(mr);
  }
  XLOGF(INFO, "dereg finish");
  exit = true;
}

TEST_F(TestIBDevice, DISABLED_VerbsRegMR) {
  int num_devices;
  struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
  SCOPE_EXIT { ibv_free_device_list(dev_list); };
  ASSERT_NE(dev_list, nullptr);
  XLOGF(INFO, "Use device {}", ibv_get_device_name(dev_list[0]));

  auto dev = dev_list[0];
  auto ctx = ibv_open_device(dev);
  ASSERT_NE(ctx, nullptr);
  auto pd = ibv_alloc_pd(ctx);
  ASSERT_NE(pd, nullptr);

  // malloc
  auto memsize = FLAGS_reg_gb * (1ULL << 30);
  XLOGF(INFO, "malloc begin {}", memsize);
  auto *mem = (uint8_t *)malloc(memsize);
  XLOGF(INFO, "malloc success, {} {}", memsize, (void *)mem);

  // ibv_reg_mr
  std::vector<ibv_mr *> mrs;
  auto bs = memsize / FLAGS_reg_split;
  XLOGF(INFO, "reg begin");
  for (size_t i = 0; i < FLAGS_reg_split; i++) {
    auto mr = ibv_reg_mr(pd, mem + bs * i, bs, regFlags());
    XLOGF(INFO, "reg get {}", (void *)mr);
    ASSERT_NE(mr, nullptr) << memsize;
    mrs.push_back(mr);
  }

  XLOGF(INFO, "dereg begin");
  for (auto mr : mrs) {
    ibv_dereg_mr(mr);
  }
  XLOGF(INFO, "dereg finish");
}

// on CI machine with virtualized RDMA NIC, this test will fail.
TEST_F(TestIBDevice, DISABLED_reregMR) {
  static constexpr size_t kMemFlags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
  for (auto i = 0; i < 100; i++) {
    void *mem = malloc(4 << 10);
    auto guard = folly::makeGuard([&]() { free(mem); });

    auto mr1 = IBDevice::get(0)->regMemory(mem, 4 << 10, kMemFlags);
    auto rkey1 = mr1->rkey;
    ASSERT_EQ(IBDevice::get(0)->deregMemory(mr1), 0);
    auto mr2 = IBDevice::get(0)->regMemory(mem, 4 << 10, kMemFlags);
    ASSERT_NE(rkey1, mr2->rkey);
  }
}

}  // namespace hf3fs::net
