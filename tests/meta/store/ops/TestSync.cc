#include <algorithm>
#include <fcntl.h>
#include <folly/Random.h>
#include <folly/Synchronized.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Task.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <mutex>
#include <optional>
#include <string>

#include "client/storage/StorageClient.h"
#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/storage/Common.h"
#include "gtest/gtest.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/ops/BatchOperation.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {
namespace {

template <typename KV>
class TestSync : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestSync, KVTypes);

TYPED_TEST(TestSync, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();
    auto &fileHelper = cluster.meta().getFileHelper();
    auto &storage = cluster.meta().getStorageClient();

    for (size_t i = 0; i < 10; i++) {
      bool dynStripe = i % 2;
      auto create = co_await meta.create(
          {SUPER_USER, PathAt(fmt::format("test-file-{}", i)), {}, O_EXCL, p644, std::nullopt, dynStripe});
      CO_ASSERT_OK(create);
      auto inode = create->stat;
      CO_ASSERT_EQ(inode.asFile().length, 0);
      CO_ASSERT_EQ(inode.asFile().dynStripe != 0, dynStripe);

      // sync directory
      CO_ASSERT_ERROR(co_await meta.sync({SUPER_USER, InodeId::root(), true, std::nullopt, std::nullopt}),
                      MetaCode::kNotFile);

      // sync empty file, length should be 0
      auto result = co_await meta.sync({SUPER_USER, inode.id, true, {}, {}});
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->stat.asFile().length, 0);

      // write at random offset, sync and check new length
#ifdef GTEST_ATTRIBUTE_NO_SANITIZE_ADDRESS_
      for (size_t i = 0; i < 50; i++) {
        auto offset = folly::Random::rand64(10_GB);
        auto length = folly::Random::rand64(2_MB);
#else
      for (size_t i = 0; i < 10; i++) {
        auto offset = folly::Random::rand64(100_MB);
        auto length = folly::Random::rand64(1_MB);
#endif
        auto expectedLength = std::max(inode.asFile().length, offset + length);
        co_await randomWrite(meta, storage, inode, offset, length);

        auto stat = co_await meta.stat({SUPER_USER, inode.id, AtFlags()});
        CO_ASSERT_OK(stat);

        auto newLength = co_await fileHelper.queryLength(SUPER_USER, stat->stat, nullptr);
        CO_ASSERT_OK(newLength);
        CO_ASSERT_EQ(expectedLength, *newLength);
        inode.asFile().length = expectedLength;

        {
          // sync file
          auto result = co_await meta.sync({SUPER_USER, inode.id, true, {}, {}});
          CO_ASSERT_OK(result);
          CO_ASSERT_EQ(result->stat.asFile().length, *newLength);
        }

        // stat file
        auto result = co_await meta.stat({SUPER_USER, inode.id, AtFlags(AT_SYMLINK_FOLLOW)});
        CO_ASSERT_OK(result);
        CO_ASSERT_EQ(result->stat.asFile().length, *newLength);
      }
    }
  }());
}

TYPED_TEST(TestSync, Concurrent) {
  auto cluster = this->createMockCluster();
  auto &meta = cluster.meta().getOperator();
  auto &fileHelper = cluster.meta().getFileHelper();
  auto &storage = cluster.meta().getStorageClient();

  // start multiple workers to truncate this file
  auto worker = [&](Inode &inode) -> CoTask<void> {
#ifdef GTEST_ATTRIBUTE_NO_SANITIZE_ADDRESS_
    for (size_t i = 0; i < folly::Random::rand32(100); i++) {
      auto offset = folly::Random::rand64(500_MB);
      auto length = folly::Random::rand64(2_MB);
#else
    for (size_t i = 0; i < folly::Random::rand32(10); i++) {
      auto offset = folly::Random::rand64(500_MB);
      auto length = folly::Random::rand64(1_MB);
#endif
      CO_ASSERT_NE(inode.id, InodeId());
      CO_ASSERT_TRUE(inode.isFile());
      auto newLength = std::max(inode.asFile().length, offset + length);
      if (folly::Random::oneIn(50)) {
        co_await truncate(meta, storage, inode, newLength);
      } else {
        co_await randomWrite(meta, storage, inode, offset, length);
      }
    }

    CO_ASSERT_OK(co_await meta.sync({SUPER_USER, inode.id, true, {}, {}}));
  };
  folly::CPUThreadPoolExecutor exec(8);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto create = co_await meta.create({SUPER_USER, PathAt("test-file"), {}, O_EXCL, p644});
    CO_ASSERT_OK(create);
    Inode inode = create->stat;
    CO_ASSERT_TRUE(inode.isFile());
    CO_ASSERT_EQ(inode.asFile().length, 0);

    std::vector<folly::SemiFuture<folly::Unit>> futures;
    for (size_t i = 0; i < 64; i++) {
      futures.push_back(folly::coro::co_invoke(worker, inode).scheduleOn(&exec).start());
    }
    for (auto &f : futures) {
      f.wait();
    }
    auto newLength = co_await fileHelper.queryLength(SUPER_USER, inode, nullptr);
    CO_ASSERT_OK(newLength);

    auto result = co_await meta.stat({SUPER_USER, inode.id, AtFlags(AT_SYMLINK_FOLLOW)});
    CO_ASSERT_OK(result);
    CO_ASSERT_TRUE(result->stat.isFile());
    CO_ASSERT_EQ(result->stat.asFile().length, *newLength);
  }());
}

TYPED_TEST(TestSync, ConflictSet) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.set_num_meta(1);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();
    auto &store = cluster.meta().getStore();

    auto create = co_await meta.create({SUPER_USER, PathAt("test-file"), {}, O_EXCL, p644});
    CO_ASSERT_OK(create);
    InodeId inodeId = create->stat.id;

    BatchedOp::Waiter<SyncReq, SyncRsp> waiter(
        SyncReq(SUPER_USER, inodeId, true, {}, {}, false, VersionedLength{10, 0}));
    CHECK_CONFLICT_SET(
        [&](auto &txn) -> CoTask<void> {
          BatchedOp op(store, inodeId);
          op.add(waiter);
          CO_ASSERT_OK(co_await op.run(txn));
        },
        (std::vector<String>{MetaTestHelper::getInodeKey(inodeId)}),
        (std::vector<String>{MetaTestHelper::getInodeKey(inodeId)}),
        false);
  }());
}

TYPED_TEST(TestSync, hint) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    auto create = co_await meta.create({SUPER_USER, PathAt("test-file"), {}, O_EXCL, p644});
    CO_ASSERT_OK(create);
    InodeId inodeId = create->stat.id;

    // invalid hint, DFATAL
    // CO_ASSERT_ERROR(co_await meta.sync({SUPER_USER, inodeId, true, std::nullopt, std::nullopt, false,
    // VersionedLength{100, 10}}), MetaCode::kFoundBug);

    // sync truncate
    auto r1 =
        co_await meta.sync({SUPER_USER, inodeId, true, std::nullopt, std::nullopt, true, VersionedLength{100, 0}});
    CO_ASSERT_OK(r1);
    CO_ASSERT_EQ(r1->stat.asFile().getVersionedLength(), (VersionedLength{0, 1}));

    // sync with hint
    auto r2 =
        co_await meta.sync({SUPER_USER, inodeId, true, std::nullopt, std::nullopt, false, VersionedLength{1024, 1}});
    CO_ASSERT_OK(r2);
    CO_ASSERT_EQ(r2->stat.asFile().getVersionedLength(), (VersionedLength{1024, 1}));

    // sync with outdate hint
    auto r3 =
        co_await meta.sync({SUPER_USER, inodeId, true, std::nullopt, std::nullopt, false, VersionedLength{6000, 0}});
    CO_ASSERT_OK(r3);
    CO_ASSERT_EQ(r3->stat.asFile().getVersionedLength(), (VersionedLength{0, 2}));

    // sync with hint
    auto r4 =
        co_await meta.sync({SUPER_USER, inodeId, true, std::nullopt, std::nullopt, false, VersionedLength{6000, 2}});
    CO_ASSERT_OK(r4);
    CO_ASSERT_EQ(r4->stat.asFile().getVersionedLength(), (VersionedLength{6000, 2}));

    // close
    auto r5 =
        co_await meta.close({SUPER_USER, inodeId, MetaTestHelper::randomSession(), true, std::nullopt, std::nullopt});
    CO_ASSERT_OK(r5);
    CO_ASSERT_EQ(r5->stat.asFile().getVersionedLength(), (VersionedLength{0, 3}));
  }());
}

}  // namespace
}  // namespace hf3fs::meta::server
