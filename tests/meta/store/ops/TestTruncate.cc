#include <atomic>
#include <fcntl.h>
#include <folly/Random.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Task.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "gtest/gtest.h"
#include "meta/components/FileHelper.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {
namespace {

template <typename KV>
class TestTruncate : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestTruncate, KVTypes);

TYPED_TEST(TestTruncate, Basic) {
  auto cluster = this->createMockCluster();
  auto &meta = cluster.meta().getOperator();
  auto &fileHelper = cluster.meta().getFileHelper();
  auto &storage = cluster.meta().getStorageClient();
  auto truncateAndStat = [&](Inode &inode, uint64_t targetLength) -> CoTask<void> {
    co_await truncate(meta, storage, inode, targetLength);

    // do stat, should get new length.
    auto result = co_await meta.stat({SUPER_USER, PathAt("test-file"), AtFlags(AT_SYMLINK_FOLLOW)});
    CO_ASSERT_OK(result);
    CO_ASSERT_EQ(result->stat.asFile().length, targetLength);

    // query from file helper, should get same length.
    auto newLength = co_await fileHelper.queryLength(SUPER_USER, inode, nullptr);
    CO_ASSERT_OK(newLength);
    CO_ASSERT_EQ(*newLength, targetLength);
  };

  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto create =
        co_await meta.create({SUPER_USER, "test-file", {}, O_EXCL, p644, Layout::newEmpty(ChainTableId(1), 1_KB, 128)});
    CO_ASSERT_OK(create);
    Inode inode(create->stat);
    CO_ASSERT_EQ(inode.asFile().length, 0);

    // file length should be zero
    auto newLength = co_await fileHelper.queryLength(SUPER_USER, inode, nullptr);
    CO_ASSERT_OK(newLength);
    CO_ASSERT_EQ(*newLength, 0);

    // truncate file up
    co_await truncateAndStat(inode, 2_MB + 13);
    co_await truncateAndStat(inode, 5_MB - 1);

    // truncate file down
    co_await truncateAndStat(inode, 2_MB - 1023);
    co_await truncateAndStat(inode, 512_KB - 1);
    co_await truncateAndStat(inode, 0);

    for (size_t i = 0; i < 10; i++) {
      co_await truncateAndStat(inode, folly::Random::rand64(2_MB));
    }
  }());
}

TYPED_TEST(TestTruncate, Deperated) {
  auto cluster = this->createMockCluster();
  auto &meta = cluster.meta().getOperator();

  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto create =
        co_await meta.create({SUPER_USER, "test-file", {}, O_EXCL, p644, Layout::newEmpty(ChainTableId(1), 1_KB, 1)});
    CO_ASSERT_OK(create);
    Inode inode(create->stat);
    CO_ASSERT_EQ(inode.asFile().length, 0);
    CO_ASSERT_ERROR(co_await meta.truncate({SUPER_USER, inode.id, 512, 1}), StatusCode::kNotImplemented);
  }());
}

TYPED_TEST(TestTruncate, Concurrent) {
  auto cluster = this->createMockCluster();
  auto &meta = cluster.meta().getOperator();
  auto &fileHelper = cluster.meta().getFileHelper();
  auto &storage = cluster.meta().getStorageClient();

  folly::CPUThreadPoolExecutor exec(8);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    // start multiple workers to truncate this file
    auto create = co_await meta.create({SUPER_USER, PathAt("test-file"), {}, O_EXCL, p644});
    CO_ASSERT_OK(create);
    auto inode = create->stat;
    CO_ASSERT_EQ(inode.asFile().length, 0);

    auto worker = [&]() -> CoTask<void> {
      if (folly::Random::oneIn(4)) {
        co_await truncate(meta, storage, inode, 0);
      } else {
        co_await truncate(meta, storage, inode, folly::Random::rand64(10_MB, 20_MB));
      }
    };

    std::vector<folly::SemiFuture<folly::Unit>> futures;
    for (size_t i = 0; i < 64; i++) {
      futures.push_back(folly::coro::co_invoke(worker).scheduleOn(&exec).start());
    }
    for (auto &f : futures) {
      f.wait();
    }

    auto result = co_await meta.stat({SUPER_USER, PathAt("test-file"), AtFlags(AT_SYMLINK_FOLLOW)});
    CO_ASSERT_OK(result);
    inode = result->stat;
    auto length = inode.asFile().length;
    CO_ASSERT_TRUE(length < 20_MB) << length;

    auto newLength = co_await fileHelper.queryLength(SUPER_USER, inode, nullptr);
    CO_ASSERT_OK(newLength);
    std::cout << fmt::format("{}", inode.asFile().getVersionedLength()) << " " << *newLength << std::endl;
    CO_ASSERT_EQ(length, *newLength);
  }());
}

// TYPED_TEST(TestTruncate, ConflictSet) {
//   folly::coro::blockingWait([&]() -> CoTask<void> {
//     auto cluster = this->createMockCluster();
//     auto &store = cluster.meta().getStore();

//     auto create = co_await cluster.meta().getOperator().create({SUPER_USER, PathAt("test-file"), {}, O_EXCL, p644});
//     CO_ASSERT_OK(create);
//     InodeId inodeId = create->stat.id;

//     CHECK_CONFLICT_SET(
//         [&](auto &txn) -> CoTask<void> {
//           auto req = TruncateReq(SUPER_USER, inodeId, 1_MB, 32);
//           CO_ASSERT_OK(co_await store.truncate(req)->run(txn));
//         },
//         (std::vector<String>{MetaTestHelper::getInodeKey(inodeId)}),
//         (std::vector<String>{MetaTestHelper::getInodeKey(inodeId)}),
//         false);
//   }());
// }

}  // namespace
}  // namespace hf3fs::meta::server
