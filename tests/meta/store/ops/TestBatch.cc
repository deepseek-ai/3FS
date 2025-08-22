#include <algorithm>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/Synchronized.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/Task.h>
#include <folly/futures/Barrier.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <tuple>
#include <vector>

#include "client/storage/StorageClient.h"
#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "common/utils/UtcTime.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "fbs/storage/Common.h"
#include "fdb/FDBRetryStrategy.h"
#include "gtest/gtest.h"
#include "meta/components/SessionManager.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/ops/BatchOperation.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

template <typename KV>
class TestBatchOp : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestBatchOp, KVTypes);

TYPED_TEST(TestBatchOp, basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.set_num_meta(1);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();
    auto &store = cluster.meta().getStore();
    auto &engine = *this->kvEngine();

    // create a inodeId to test
    auto create = co_await meta.create({SUPER_USER, PathAt("test"), {}, O_EXCL, p644});
    CO_ASSERT_OK(create);
    auto inodeId = create->stat.id;

    auto u1 = flat::UserInfo(flat::Uid(1), flat::Gid(1), "");
    auto u2 = flat::UserInfo(flat::Uid(2), flat::Gid(2), "");

    auto waiter1 = BatchedOp::Waiter<SetAttrReq, SetAttrRsp>(
        SetAttrReq::setPermission(SUPER_USER, PathAt(inodeId), AtFlags(), flat::Uid(1), std::nullopt, std::nullopt));
    auto waiter2 = BatchedOp::Waiter<SetAttrReq, SetAttrRsp>(
        SetAttrReq::setPermission(u1, PathAt(inodeId), AtFlags(), std::nullopt, flat::Gid(1), std::nullopt));
    auto waiter3 = BatchedOp::Waiter<SetAttrReq, SetAttrRsp>(
        SetAttrReq::setPermission(u2, PathAt(inodeId), AtFlags(), std::nullopt, flat::Gid(2), std::nullopt));
    auto waiter4 = BatchedOp::Waiter<SyncReq, SyncRsp>(
        SyncReq(SUPER_USER, inodeId, true, SETATTR_TIME_NOW, SETATTR_TIME_NOW, false, meta::VersionedLength{1000, 0}));
    auto waiter5 = BatchedOp::Waiter<CloseReq, CloseRsp>(
        CloseReq(SUPER_USER, inodeId, MetaTestHelper::randomSession(), true, std::nullopt, std::nullopt));
    auto waiter6 = BatchedOp::Waiter<CloseReq, CloseRsp>(
        CloseReq(SUPER_USER, inodeId, std::nullopt, true, std::nullopt, std::nullopt));

    {
      FAULT_INJECTION_SET(10, 3);
      auto batch = std::make_unique<BatchedOp>(store, inodeId);
      batch->add(waiter1);
      batch->add(waiter2);
      batch->add(waiter3);
      batch->add(waiter4);
      batch->retry(Status(StatusCode::kInvalidArg));

      auto txn = engine.createReadWriteTransaction();
      auto driver = OperationDriver(*batch, Void{});
      auto result = co_await driver.run(std::move(txn), {}, false, false);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->asFile().getVersionedLength(), (meta::VersionedLength{1000, 0}));
      CO_ASSERT_EQ(result->acl.uid, flat::Uid(1));
      CO_ASSERT_EQ(result->acl.gid, flat::Gid(1));

      CO_ASSERT_OK(waiter1.getResult());
      CO_ASSERT_OK(waiter2.getResult());
      CO_ASSERT_ERROR(waiter3.getResult(), MetaCode::kNoPermission);
      CO_ASSERT_OK(waiter4.getResult());
    };

    {
      FAULT_INJECTION_SET(10, 3);
      auto batch = std::make_unique<BatchedOp>(store, inodeId);
      batch->add(waiter4);
      batch->add(waiter5);
      batch->add(waiter6);
      batch->retry(Status(StatusCode::kInvalidArg));

      auto txn = engine.createReadWriteTransaction();
      auto driver = OperationDriver(*batch, Void{});
      auto result = co_await driver.run(std::move(txn), {}, false, false);
      if (!result.hasError()) {
        CO_ASSERT_EQ(result->asFile().getVersionedLength(), (meta::VersionedLength{0, 1}));
        CO_ASSERT_OK(waiter4.getResult());
        CO_ASSERT_OK(waiter5.getResult());
        CO_ASSERT_ERROR(waiter6.getResult(), StatusCode::kInvalidArg);
      }
    };
  }());
}

TYPED_TEST(TestBatchOp, batch) {
  auto cluster = this->createMockCluster();
  auto &meta = cluster.meta().getOperator();

  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto req1 = SyncReq(SUPER_USER, InodeId(1), true, {}, {});
    auto req2 = SyncReq(SUPER_USER, InodeId(2), true, {}, {});

    BatchedOp::Waiter<SyncReq, SyncRsp> waiter1(req1), waiter2(req1), waiter3(req1), waiter4(req2), waiter5(req1);
    auto batch1 = meta.addBatchReq(InodeId(1), waiter1);
    CO_ASSERT_NE(batch1.get(), nullptr);
    auto batch2 = meta.addBatchReq(InodeId(1), waiter2);
    CO_ASSERT_NE(batch2.get(), nullptr);
    auto batch3 = meta.addBatchReq(InodeId(1), waiter3);
    CO_ASSERT_EQ(batch3.get(), nullptr);
    auto batch4 = meta.addBatchReq(InodeId(2), waiter4);
    CO_ASSERT_NE(batch4.get(), nullptr);
    CO_ASSERT_NE(batch1.get(), batch4.get());

    // batch 1 can run, but batch 2 can't
    CO_ASSERT_TRUE(waiter1.baton.ready());
    CO_ASSERT_FALSE(waiter2.baton.ready());
    CO_ASSERT_OK(co_await meta.runBatch(InodeId(1), std::move(batch1)));
    CO_ASSERT_TRUE(waiter2.baton.ready());
    CO_ASSERT_FALSE(waiter3.baton.ready());
    CO_ASSERT_OK(co_await meta.runBatch(InodeId(1), std::move(batch2)));
    CO_ASSERT_TRUE(waiter3.baton.ready());

    auto batch5 = meta.addBatchReq(InodeId(1), waiter5);
    CO_ASSERT_NE(batch5.get(), nullptr);
    CO_ASSERT_TRUE(waiter5.baton.ready());

    std::unique_ptr<BatchedOp> batch6;
    std::vector<std::unique_ptr<BatchedOp::Waiter<SyncReq, SyncRsp>>> waiters;
    for (size_t i = 0; i < 5000; i++) {
      auto waiter = std::make_unique<BatchedOp::Waiter<SyncReq, SyncRsp>>(req1);
      auto batch = meta.addBatchReq(InodeId(1), *waiter);
      if (!batch6) {
        CO_ASSERT_TRUE(batch);
        batch6 = std::move(batch);
      }
      if (i >= cluster.config().mock_meta().max_batch_operations()) {
        CO_ASSERT_TRUE(waiter->baton.ready());
        CO_ASSERT_EQ(waiter->getResult().error().code(), MetaCode::kBusy);
      } else {
        CO_ASSERT_FALSE(waiter->baton.ready()) << fmt::format("{}", i);
      }
      waiters.push_back(std::move(waiter));
    }

    // reject timeout operation
    CO_ASSERT_ERROR(co_await meta.runBatch(InodeId(1), std::move(batch5), SteadyClock::now()),
                    MetaCode::kOperationTimeout);
  }());
}

}  // namespace hf3fs::meta::server