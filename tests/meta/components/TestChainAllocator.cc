#include <folly/Random.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <gflags/gflags.h>
#include <gtest/gtest-param-test.h>
#include <map>

#include "common/utils/Coroutine.h"
#include "fbs/meta/Common.h"
#include "fbs/mgmtd/ChainRef.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "gtest/gtest.h"
#include "meta/components/ChainAllocator.h"
#include "tests/meta/MetaTestBase.h"

DEFINE_int64(chain_alloc_test, 50000, "files in test chain allocator");

namespace hf3fs::meta::server {

template <typename KV>
class TestChainAllocator : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestChainAllocator, KVTypes);

TYPED_TEST(TestChainAllocator, basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto mgmtd = cluster.mgmtdClient();
    ChainAllocator alloc(mgmtd);

    auto empty = Layout::newEmpty(flat::ChainTableId(1), flat::ChainTableVersion(0), 512 << 10, 16);
    CO_ASSERT_OK(co_await alloc.checkLayoutValid(empty));
    CO_ASSERT_OK(co_await alloc.allocateChainsForLayout(empty));
    CO_ASSERT_TRUE(empty.valid(false));
  }());
}

TYPED_TEST(TestChainAllocator, perDirCounter) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();
    auto mgmtd = cluster.mgmtdClient();
    ChainAllocator alloc(mgmtd);

    CO_ASSERT_OK(
        co_await meta.setAttr(SetAttrReq::setIFlags(SUPER_USER, InodeId::root(), IFlags(FS_CHAIN_ALLOCATION_FL))));

    auto routing = mgmtd->getRoutingInfo();
    auto chainCount = routing->raw()->getChainTable(flat::ChainTableId(1), flat::ChainTableVersion(0))->chains.size();

    std::optional<size_t> prevChain;
    for (size_t i = 0; i < 1000; i++) {
      auto result = co_await meta.create({SUPER_USER, folly::to<std::string>(i), std::nullopt, OpenFlags(), p644});
      CO_ASSERT_OK(result);
      auto layout = result->stat.asFile().layout;
      CO_ASSERT_OK(co_await alloc.checkLayoutValid(layout));
      auto chain = std::get<Layout::ChainRange>(layout.chains).baseIndex;
      if (prevChain) {
        CO_ASSERT_EQ(chain % chainCount, (*prevChain + layout.stripeSize) % chainCount);
      }
      prevChain = chain;
    }
  }());
}

TYPED_TEST(TestChainAllocator, balance) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto mgmtd = cluster.mgmtdClient();
    ChainAllocator alloc(mgmtd);

    for (auto tableId :
         std::vector<flat::ChainTableId>{flat::ChainTableId(1), flat::ChainTableId(2), flat::ChainTableId(3)}) {
      auto routing = mgmtd->getRoutingInfo();
      auto chainCount = routing->raw()->getChainTable(tableId, flat::ChainTableVersion(0))->chains.size();

      std::map<flat::ChainId, size_t> map;
      std::vector<size_t> stripeSizes{1, 2, 4, 8, 30, 32, 60, 64, 120, 128, 240, 256, 512};
      for (int i = 0; i < FLAGS_chain_alloc_test; i++) {
        auto stripe = 1000000u;
        while (stripe > chainCount) {
          stripe = stripeSizes.at(folly::Random::rand64(stripeSizes.size()));
        }
        auto empty = Layout::newEmpty(tableId, 512 << 10, stripe);
        auto emptyCheck = co_await alloc.checkLayoutValid(empty);
        CO_ASSERT_OK(emptyCheck);
        CO_ASSERT_OK(co_await alloc.allocateChainsForLayout(empty));
        CO_ASSERT_TRUE(empty.valid(false));
        CO_ASSERT_OK(co_await alloc.checkLayoutValid(empty));

        size_t begin = chainCount;
        std::set<flat::ChainId> set;
        for (auto chain : empty.getChainIndexList()) {
          auto index = chain % chainCount ? chain % chainCount : chainCount;
          auto ref = flat::ChainRef(empty.tableId, empty.tableVersion, index);
          auto id = routing->raw()->getChainId(ref);
          CO_ASSERT_TRUE(id.has_value());
          CO_ASSERT_FALSE(set.contains(*id));
          set.emplace(*id);
          map[*id]++;
          begin = std::min(index, begin);
        }

        if (chainCount % stripe == 0 && stripe != 1) {
          CO_ASSERT_EQ(begin % stripe, 1)
              << fmt::format("{} {} {}", stripe, begin, fmt::join(set.begin(), set.end(), ","));
        }
      }

      size_t min = map.begin()->second, max = map.begin()->second;
      for (auto [id, cnt] : map) {
        (void)id;
        min = std::min(cnt, min);
        max = std::max(cnt, max);
      }
      CO_ASSERT_EQ(map.size(), chainCount);
      CO_ASSERT_GE(min + stripeSizes.size(), max);
    }
  }());
}

}  // namespace hf3fs::meta::server
