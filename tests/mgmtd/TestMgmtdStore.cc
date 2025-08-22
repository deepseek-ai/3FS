#include <folly/experimental/coro/BlockingWait.h>

#include "common/kv/mem/MemKVEngine.h"
#include "mgmtd/store/MgmtdStore.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::testing {
namespace {
using namespace ::hf3fs::mgmtd;

class MgmtdStoreTest : public ::testing::Test {
 protected:
  MgmtdStoreTest() {
    node0.nodeId = flat::NodeId(0);
    node1.nodeId = flat::NodeId(1);
  }

  flat::PersistentNodeInfo from(flat::NodeId id, flat::PersistentNodeInfo info) {
    info.nodeId = id;
    return info;
  }

  kv::MemKVEngine engine;
  UtcTime now;
  std::function<UtcTime()> generator = [this] { return now; };
  flat::PersistentNodeInfo node0;
  flat::PersistentNodeInfo node1;
  MgmtdStore store;
  static constexpr auto leaseLength = std::chrono::microseconds(1000);
};

TEST_F(MgmtdStoreTest, testExtendLease) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    {
      // nobody holds the lease
      now = UtcTime::fromMicroseconds(10000);
      auto txn = engine.createReadWriteTransaction();
      auto result = co_await store.extendLease(*txn, node0, leaseLength, now);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->primary.nodeId, 0);
      CO_ASSERT_EQ(result->leaseStart.toMicroseconds(), 10000);
      CO_ASSERT_EQ(result->leaseEnd.toMicroseconds(), 11000);
      CO_AWAIT_ASSERT_OK(txn->commit());
    }
    {
      // lease won't go back
      now = UtcTime::fromMicroseconds(9900);
      auto txn = engine.createReadWriteTransaction();
      auto result = co_await store.extendLease(*txn, node1, leaseLength, now);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->primary.nodeId, node0.nodeId);
      CO_ASSERT_EQ(result->leaseStart.toMicroseconds(), 10000);
      CO_ASSERT_EQ(result->leaseEnd.toMicroseconds(), 11000);
      CO_AWAIT_ASSERT_OK(txn->commit());
    }
    {
      // node1 won't become the lease owner before lease is end
      now = UtcTime::fromMicroseconds(10900);
      auto txn = engine.createReadWriteTransaction();
      auto result = co_await store.extendLease(*txn, node1, leaseLength, now);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->primary.nodeId, node0.nodeId);
      CO_ASSERT_EQ(result->leaseStart.toMicroseconds(), 10000);
      CO_ASSERT_EQ(result->leaseEnd.toMicroseconds(), 11000);
      CO_AWAIT_ASSERT_OK(txn->commit());
    }
    {
      // node1 becomes the lease owner after lease is end
      now = UtcTime::fromMicroseconds(11100);
      auto txn = engine.createReadWriteTransaction();
      auto result = co_await store.extendLease(*txn, node1, leaseLength, now);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->primary.nodeId, node1.nodeId);
      CO_ASSERT_EQ(result->leaseStart.toMicroseconds(), 11100);
      CO_ASSERT_EQ(result->leaseEnd.toMicroseconds(), 12100);
      CO_AWAIT_ASSERT_OK(txn->commit());
    }
    {
      // node1 extends lease
      now = UtcTime::fromMicroseconds(11500);
      auto txn = engine.createReadWriteTransaction();
      auto result = co_await store.extendLease(*txn, node1, leaseLength, now);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->primary.nodeId, node1.nodeId);
      CO_ASSERT_EQ(result->leaseStart.toMicroseconds(), 11100);
      CO_ASSERT_EQ(result->leaseEnd.toMicroseconds(), 12500);
      CO_AWAIT_ASSERT_OK(txn->commit());
    }
    {
      // node1 starts new lease after the previous lease was finished
      now = UtcTime::fromMicroseconds(13000);
      auto txn = engine.createReadWriteTransaction();
      auto result = co_await store.extendLease(*txn, node1, leaseLength, now);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->primary.nodeId, node1.nodeId);
      CO_ASSERT_EQ(result->leaseStart.toMicroseconds(), 13000);
      CO_ASSERT_EQ(result->leaseEnd.toMicroseconds(), 14000);
      CO_AWAIT_ASSERT_OK(txn->commit());
    }
    {
      // node1 cannot extend lease with a stale release version
      auto rv = flat::ReleaseVersion::fromVersionInfo();
      --rv.buildTimeInSeconds;
      now = UtcTime::fromMicroseconds(13001);
      auto txn = engine.createReadWriteTransaction();
      auto result = co_await store.extendLease(*txn, node1, leaseLength, now, rv, true);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->primary.nodeId, node1.nodeId);
      CO_ASSERT_EQ(result->leaseStart.toMicroseconds(), 13000);
      CO_ASSERT_EQ(result->leaseEnd.toMicroseconds(), 14000);
      CO_AWAIT_ASSERT_OK(txn->commit());
    }
    co_return;
  }());
}

TEST_F(MgmtdStoreTest, testEnsureLeaseValid) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    {
      // let node0 get the lease
      now = UtcTime::fromMicroseconds(10000);
      auto txn = engine.createReadWriteTransaction();
      auto result = co_await store.extendLease(*txn, node0, leaseLength, now);
      CO_ASSERT_OK(result);
      CO_AWAIT_ASSERT_OK(txn->commit());
    }
    {
      // node1 got "not primary"
      auto txn = engine.createReadonlyTransaction();
      CO_AWAIT_ASSERT_ERROR(MgmtdCode::kNotPrimary, store.ensureLeaseValid(*txn, node1.nodeId, now));
    }
    {
      // node0 ensures self holding the lease
      now = UtcTime::fromMicroseconds(10500);
      auto txn = engine.createReadonlyTransaction();
      CO_AWAIT_ASSERT_OK(store.ensureLeaseValid(*txn, node0.nodeId, now));
    }
    {
      // node0 got "not primary" after lease is expired
      now = UtcTime::fromMicroseconds(11500);
      auto txn = engine.createReadonlyTransaction();
      CO_AWAIT_ASSERT_ERROR(MgmtdCode::kNotPrimary, store.ensureLeaseValid(*txn, node0.nodeId, now));
    }
    co_return;
  }());
}

TEST_F(MgmtdStoreTest, testLoadStoreNode) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    {
      // node0 got lease, then store will succeed
      flat::PersistentNodeInfo ni = {
          .type = flat::NodeType::META,
          .serviceGroups = {flat::ServiceGroupInfo({"meta0", "meta1"}, {net::Address::fromString("127.0.0.1:9876")})},
      };

      now = UtcTime::fromMicroseconds(10000);
      auto txn = engine.createReadWriteTransaction();
      CO_AWAIT_ASSERT_OK(store.extendLease(*txn, node0, leaseLength, now));
      CO_AWAIT_ASSERT_OK(txn->commit());

      txn = engine.createReadWriteTransaction();
      CO_AWAIT_ASSERT_OK(store.storeNodeInfo(*txn, from(flat::NodeId(2), ni)));
      CO_AWAIT_ASSERT_OK(store.storeNodeInfo(*txn, from(flat::NodeId(3), ni)));
      CO_AWAIT_ASSERT_OK(store.storeNodeInfo(*txn, from(flat::NodeId(4), ni)));
      CO_AWAIT_ASSERT_OK(txn->commit());

      // store now could load node info without lease ownership
      auto txn1 = engine.createReadonlyTransaction();
      auto result = co_await store.loadNodeInfo(*txn1, flat::NodeId(2));
      CO_ASSERT_OK(result);
      CO_ASSERT_TRUE(result->has_value());
      CO_ASSERT_EQ(from(flat::NodeId(2), ni), result->value());

      kv::mem::memKvDefaultLimit = 1;
      auto result1 = co_await store.loadAllNodes(*txn1);
      CO_ASSERT_OK(result1);
      CO_ASSERT_EQ(result1->size(), 3);
      CO_ASSERT_EQ(from(flat::NodeId(2), ni), result1->at(0));
      CO_ASSERT_EQ(from(flat::NodeId(3), ni), result1->at(1));
      CO_ASSERT_EQ(from(flat::NodeId(4), ni), result1->at(2));
    }
    co_return;
  }());
}

TEST_F(MgmtdStoreTest, testLoadStoreConfig) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto txn = engine.createReadWriteTransaction();
    auto loadAllRes = co_await store.loadAllConfigs(*txn);
    CO_ASSERT_OK(loadAllRes);
    CO_ASSERT_TRUE(loadAllRes->empty());

    auto cfg = flat::ConfigInfo::create(flat::ConfigVersion(5), "abc");
    CO_AWAIT_ASSERT_OK(
        store.storeConfig(*txn, flat::NodeType::MGMTD, flat::ConfigInfo::create(flat::ConfigVersion(5), "abc")));
    CO_AWAIT_ASSERT_OK(
        store.storeConfig(*txn, flat::NodeType::MGMTD, flat::ConfigInfo::create(flat::ConfigVersion(6), "abcd")));

    auto loadRes = co_await store.loadConfig(*txn, flat::NodeType::MGMTD, flat::ConfigVersion(4));
    CO_ASSERT_OK(loadRes);
    CO_ASSERT_TRUE(!loadRes->has_value());

    loadRes = co_await store.loadConfig(*txn, flat::NodeType::MGMTD, flat::ConfigVersion(5));
    CO_ASSERT_OK(loadRes);
    CO_ASSERT_TRUE(loadRes->has_value());
    CO_ASSERT_EQ(loadRes->value().configVersion, 5);
    CO_ASSERT_EQ(loadRes->value().content, "abc");

    loadRes = co_await store.loadConfig(*txn, flat::NodeType::MGMTD, flat::ConfigVersion(6));
    CO_ASSERT_OK(loadRes);
    CO_ASSERT_TRUE(loadRes->has_value());
    CO_ASSERT_EQ(loadRes->value().configVersion, 6);
    CO_ASSERT_EQ(loadRes->value().content, "abcd");

    loadRes = co_await store.loadLatestConfig(*txn, flat::NodeType::MGMTD);
    CO_ASSERT_OK(loadRes);
    CO_ASSERT_TRUE(loadRes->has_value());
    CO_ASSERT_EQ(loadRes->value().configVersion, 6);
    CO_ASSERT_EQ(loadRes->value().content, "abcd");
  }());
}
}  // namespace
}  // namespace hf3fs::testing
