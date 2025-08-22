#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/app/NodeId.h"
#include "common/kv/ITransaction.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/MurmurHash3.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Utils.h"
#include "gtest/gtest.h"
#include "meta/components/Distributor.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

DEFINE_uint64(distributor_iters, 10, "");

namespace hf3fs::meta::server {

#define CHECK_DIST(txn, dist, inodeId, expected)                                        \
  do {                                                                                  \
    auto result = co_await dist.checkOnServer((txn), (inodeId), (expected));            \
    CO_ASSERT_OK(result);                                                               \
    auto actual = (dist).getServer((inodeId));                                          \
    auto msg = fmt::format("{} not on {}, actual {}", (inodeId), (expected), (actual)); \
    CO_ASSERT_TRUE(result->first) << msg;                                               \
    CO_ASSERT_EQ((actual), (expected)) << msg;                                          \
  } while (0)

#define CHECK_DIST_AT(dist, inodeId, expected)                                 \
  do {                                                                         \
    READ_WRITE_TRANSACTION_OK({ CHECK_DIST(*txn, dist, inodeId, expected); }); \
  } while (0)

#define CHECK_ALL_DIST_AT(dist, expected)                  \
  do {                                                     \
    for (size_t i = 0; i < 10; i++) {                      \
      READ_WRITE_TRANSACTION_OK({                          \
        for (size_t j = 0; j < 1; j++) {                   \
          auto inodeId = InodeId(folly::Random::rand64()); \
          CHECK_DIST(*txn, dist, inodeId, expected);       \
        }                                                  \
      });                                                  \
    }                                                      \
  } while (0)

template <typename KV>
class TestDistributor : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestDistributor, KVTypes);

TYPED_TEST(TestDistributor, basic) {
  auto config = Distributor::Config();
  config.set_update_interval(1_s);
  config.set_timeout(5_s);
  auto kvEngine = this->kvEngine();
  auto exec = CPUExecutorGroup(2, "test");

  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto dist = Distributor(config, flat::NodeId(50), kvEngine);
    CHECK_ALL_DIST_AT(dist, flat::NodeId());
    dist.start(exec);
    for (size_t i = 0; i < 5; i++) {
      CHECK_ALL_DIST_AT(dist, flat::NodeId(50));
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::vector<std::unique_ptr<IReadWriteTransaction>> txns;
    for (size_t i = 0; i < 100; i++) {
      auto txn = kvEngine->createReadWriteTransaction();
      auto result = co_await dist.checkOnServer(*txn, InodeId(0));
      CO_ASSERT_OK(result);
      CO_ASSERT_TRUE(result->first);
      auto dummy = std::string(8, '\0');
      CO_ASSERT_OK(co_await txn->clear(dummy));
      txns.push_back(std::move(txn));
    }

    dist.stopAndJoin();
    for (auto &txn : txns) {
      CO_ASSERT_ERROR(co_await txn->commit(), TransactionCode::kConflict);
    }
    CHECK_ALL_DIST_AT(dist, flat::NodeId());
  }());

  exec.join();
}

TYPED_TEST(TestDistributor, balance) {
  std::map<flat::NodeId, size_t> cnts;
  std::map<InodeId, flat::NodeId> map;
  std::vector<flat::NodeId> nodes;

  auto servers = folly::Random::rand32(3, 10);
  for (size_t i = 0; i < servers; i++) {
    auto nodeId = flat::NodeId(50 + i);
    nodes.push_back(nodeId);
  }
  for (size_t i = 0; i < 10000; i++) {
    auto inodeId = InodeId(folly::Random::rand64());
    auto server = Weight::select(nodes, inodeId);
    cnts[server]++;
    map[inodeId] = server;
  }

  auto max = std::numeric_limits<size_t>::min();
  auto min = std::numeric_limits<size_t>::max();
  for (auto &[node, cnt] : cnts) {
    max = std::max(max, cnt);
    min = std::min(min, cnt);
  }
  fmt::print("min {}, max {}, avg {}\n", min, max, map.size() / nodes.size());

  nodes.erase(nodes.begin() + folly::Random::rand32(servers));

  size_t changed = 0;
  for (auto &[inodeId, server] : map) {
    auto newServer = Weight::select(nodes, inodeId);
    if (server != newServer) {
      changed++;
    }
  }
  fmt::print("changed {}, expected {}\n", changed, map.size() / (nodes.size() + 1));
}

TYPED_TEST(TestDistributor, multiple) {
  auto config = Distributor::Config();
  config.set_update_interval(100_ms);
  config.set_timeout(1_s);
  auto kvEngine = this->kvEngine();
  auto exec = CPUExecutorGroup(4, "distributor");
  auto exec2 = CPUExecutorGroup(4, "worker");

  folly::coro::blockingWait([&]() -> CoTask<void> {
    std::map<flat::NodeId, std::unique_ptr<Distributor>> distMap;
    std::map<flat::NodeId, bool> activeMap;

    for (size_t i = 0; i < 10; i++) {
      auto nodeId = flat::NodeId(50 + i);
      distMap[nodeId] = std::make_unique<Distributor>(config, nodeId, kvEngine);
      activeMap[nodeId] = false;
    }

    for (size_t i = 0; i < FLAGS_distributor_iters; i++) {
      std::vector<flat::NodeId> activeNodes;
      for (auto &iter : activeMap) {
        auto nodeId = iter.first;
        auto &active = iter.second;
        auto &dist = distMap[nodeId];
        if (folly::Random::oneIn(2)) {
          if (active) {
            auto update = folly::Random::oneIn(2);
            XLOGF(CRITICAL, "stop {}, update {}", nodeId, update);
            dist->stopAndJoin(update);
          } else {
            XLOGF(CRITICAL, "start {}", nodeId);
            dist->start(exec);
          }
          active = !active;
        }
        if (active) {
          activeNodes.push_back(nodeId);
        }
      }

      if (activeNodes.empty()) {
        continue;
      }

      std::this_thread::sleep_for(5_s);
      std::vector<folly::SemiFuture<Void>> tasks;
      for (size_t i = 0; i < 16; i++) {
        auto task = folly::coro::co_invoke([&]() -> CoTask<void> {
          for (size_t i = 0; i < 100; i++) {
            auto inodeId = InodeId(folly::Random::rand64());
            auto expected = Weight::select(activeNodes, inodeId);
            for (auto &[nodeId, dist] : distMap) {
              READ_WRITE_TRANSACTION_OK({ CHECK_DIST(*txn, (*dist), inodeId, expected); });
            }
            co_await folly::coro::co_current_executor;
          }
        });
        tasks.push_back(std::move(task).scheduleOn(&exec2.pickNext()).start());
      }
      co_await folly::coro::collectAllRange(std::move(tasks));
    }
  }());

  XLOGF(INFO, "test finished");
}

}  // namespace hf3fs::meta::server