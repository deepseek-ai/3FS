#include <chrono>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest-param-test.h>
#include <limits>
#include <map>
#include <vector>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "client/storage/StorageClientInMem.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "gtest/gtest.h"
#include "meta/components/GcManager.h"
#include "meta/components/SessionManager.h"
#include "meta/store/Inode.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

template <typename KV>
class TestGcManager : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestGcManager, KVTypes);

TYPED_TEST(TestGcManager, entry) {
  fmt::print("{}\n", GcManager::formatGcEntry('d', UtcClock::now(), InodeId(folly::Random::rand64())));
  for (size_t i = 0; i < 10000; i++) {
    auto a = UtcTime::fromMicroseconds(folly::Random::rand64(std::numeric_limits<int64_t>::max()));
    auto b = UtcTime::fromMicroseconds(folly::Random::rand64(std::numeric_limits<int64_t>::max()));
    if (a == b) continue;
    auto entryA = GcManager::formatGcEntry('f', a, InodeId::gcRoot());
    auto entryB = GcManager::formatGcEntry('f', b, InodeId::gcRoot());
    ASSERT_EQ(a < b, entryA < entryB) << fmt::format("a {}, b {}", entryA, entryB);
    auto parsed = GcManager::parseGcEntry(entryA);
    ASSERT_OK(parsed);
    ASSERT_EQ(parsed->first, a);
    ASSERT_EQ(parsed->second, InodeId::gcRoot());
  }
}

TYPED_TEST(TestGcManager, basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_remove_chunks_batch_size(1);
    config.mock_meta().gc().set_gc_directory_delay(2_s);
    config.mock_meta().gc().set_gc_file_delay(4_s);
    config.mock_meta().gc().set_scan_batch(64);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();
    auto &storage = cluster.meta().getStorageClient();
    auto mgmtd = cluster.mgmtdClient();

    std::vector<std::pair<InodeId, SessionInfo>> sessions;
    for (int i = 0; i < 10; i++) {
      auto session = MetaTestHelper::randomSession();
      auto result = co_await meta.create({SUPER_USER, std::to_string(i) + ".writing", session, O_RDWR, p644});
      CO_ASSERT_OK(result);
      co_await randomWrite(meta, storage, result->stat, folly::Random::rand64(100ULL << 30), 1ULL << 20);
      CO_ASSERT_OK(co_await meta.sync({SUPER_USER, result->stat.id, true, std::nullopt, std::nullopt}));
      sessions.push_back({result->stat.id, session});
    }

    for (int i = 0; i < 512; i++) {
      auto result = co_await meta.create({SUPER_USER, std::to_string(i) + ".file", std::nullopt, O_RDONLY, p644});
      CO_ASSERT_OK(result);
      co_await randomWrite(meta, storage, result->stat, folly::Random::rand64(100ULL << 30), 1ULL << 20);
      CO_ASSERT_OK(co_await meta.sync({SUPER_USER, result->stat.id, true, std::nullopt, std::nullopt}));
    }

    GET_INODE_CNTS(numInodes);
    GET_DIRENTRY_CNTS(numDirentries);
    CO_ASSERT_SESSION_CNTS(10);

    auto query = co_await queryTotalChunks(storage, *mgmtd);
    CO_ASSERT_OK(query);
    CO_ASSERT_NE(*query, 0);

    auto begin = UtcClock::now();
    for (int i = 0; i < 10; i++) {
      CO_ASSERT_OK(co_await meta.remove({SUPER_USER, std::to_string(i) + ".writing", AtFlags(), false}));
    }
    for (int i = 0; i < 512; i++) {
      CO_ASSERT_OK(co_await meta.remove({SUPER_USER, std::to_string(i) + ".file", AtFlags(), false}));
    }

    auto start = std::chrono::steady_clock::now();
    bool finish = false;
    while (!finish) {
      CO_ASSERT_LE(std::chrono::steady_clock::now() - start, std::chrono::seconds(60));
      co_await folly::coro::sleep(std::chrono::seconds(1));
      GET_INODE_CNTS(cnts);
      if (cnts == numInodes - 512) {
        finish = true;
      } else {
        if (cnts < numInodes) {
          CO_ASSERT_GE(UtcClock::now(), begin + 4_s);
        }
        fmt::print("{} inodes left\n", cnts);
      }
    }

    CO_ASSERT_INODE_CNTS(numInodes - 512);
    CO_ASSERT_DIRENTRY_CNTS(numDirentries - 512);

    for (auto [inode, session] : sessions) {
      CO_ASSERT_OK(co_await meta.close({SUPER_USER, inode, session, true, {}, {}}));
    }

    start = std::chrono::steady_clock::now();
    finish = false;
    while (!finish) {
      CO_ASSERT_LE(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
      co_await folly::coro::sleep(std::chrono::seconds(1));
      GET_INODE_CNTS(cnts);
      if (cnts == numInodes - 512 - 10) {
        finish = true;
      } else {
        fmt::print("{} inodes left\n", cnts);
      }
    }

    CO_ASSERT_INODE_CNTS(numInodes - 512 - 10);
    CO_ASSERT_DIRENTRY_CNTS(numDirentries - 512 - 10);
    CO_ASSERT_SESSION_CNTS(0);

    auto queryAfterRemove = co_await queryTotalChunks(storage, *mgmtd);
    CO_ASSERT_OK(queryAfterRemove);
    CO_ASSERT_EQ(*queryAfterRemove, 0);

    co_return;
  }());
}

TYPED_TEST(TestGcManager, recursive) {
  MockCluster::Config config;
  config.mock_meta().gc().set_remove_chunks_batch_size(1);
  config.mock_meta().gc().set_gc_directory_delay(3_s);
  config.mock_meta().gc().set_gc_file_delay(3_s);
  config.mock_meta().gc().set_scan_batch(128);
  auto cluster = this->createMockCluster(config);
  auto &meta = cluster.meta().getOperator();
  folly::CPUThreadPoolExecutor exec(4);
  std::vector<folly::SemiFuture<Void>> tasks;

  InodeId dir, dir1, dir2, dir3, dir4;
  folly::coro::blockingWait([&]() -> CoTask<void> {
    dir = (co_await meta.mkdirs({SUPER_USER, "dir", p644, false}))->stat.id;
    dir1 = (co_await meta.mkdirs({SUPER_USER, "dir/dir1", p644, false}))->stat.id;
    dir2 = (co_await meta.mkdirs({SUPER_USER, "dir/dir2", p644, false}))->stat.id;
    dir3 = (co_await meta.mkdirs({SUPER_USER, "dir/dir3", p644, false}))->stat.id;
    dir4 = (co_await meta.mkdirs({SUPER_USER, "dir/dir4", p644, false}))->stat.id;
  }());

  auto worker = [&meta, dir, dir1, dir2, dir3, dir4](const size_t i) -> CoTask<void> {
    for (int j = 0; j < 64; j++) {
      auto fname = fmt::format("{}.{}", i, j);
      CO_ASSERT_OK(co_await meta.create({SUPER_USER, PathAt(dir, fname), {}, O_RDONLY | O_EXCL, p644}));
      CO_ASSERT_OK(co_await meta.create({SUPER_USER, PathAt(dir1, fname), {}, O_RDONLY | O_EXCL, p644}));
      CO_ASSERT_OK(co_await meta.create({SUPER_USER, PathAt(dir2, fname), {}, O_RDONLY | O_EXCL, p644}));
      CO_ASSERT_OK(co_await meta.create({SUPER_USER, PathAt(dir3, fname), {}, O_RDONLY | O_EXCL, p644}));
      CO_ASSERT_OK(co_await meta.create({SUPER_USER, PathAt(dir4, fname), {}, O_RDONLY | O_EXCL, p644}));
      CO_ASSERT_OK(co_await meta.hardLink({SUPER_USER, PathAt(dir, fname), PathAt(dir, fname + ".1"), AtFlags()}));
    }
    co_return;
  };
  for (size_t i = 0; i < 4; i++) {
    auto task = folly::coro::co_invoke(worker, i).scheduleOn(&exec).start();
    tasks.push_back(std::move(task));
  }

  for (auto &task : tasks) {
    task.wait();
  }

  folly::coro::blockingWait([&]() -> CoTask<void> {
    GET_INODE_CNTS(numInodes);
    GET_DIRENTRY_CNTS(numEntries);

    auto expectedInodes = numInodes - 5 - 4 * 5 * 64;
    auto expectedEntries = numEntries - 5 - 4 * 6 * 64;

    auto begin = UtcClock::now();
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "dir", AtFlags(), true}));

    auto start = std::chrono::steady_clock::now();
    bool finish = false;
    while (!finish) {
      CO_ASSERT_LE(std::chrono::steady_clock::now() - start, std::chrono::seconds(120));
      co_await folly::coro::sleep(std::chrono::seconds(1));
      GET_INODE_CNTS(cnts);
      if (cnts <= expectedInodes) {
        finish = true;
      } else {
        if (cnts < numInodes - 1) {
          CO_ASSERT_GE(UtcClock::now(), begin + 6_s);
        }
        fmt::print("{} inodes left\n", cnts);
      }
    }

    CO_ASSERT_INODE_CNTS(expectedInodes);
    CO_ASSERT_DIRENTRY_CNTS(expectedEntries);
  }());
}

TYPED_TEST(TestGcManager, delayThreshold) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(true);
    config.mock_meta().gc().set_gc_file_delay(5_min);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();
    auto &storage = cluster.meta().getStorageClient();

    // create file, write then delete
    auto session = MetaTestHelper::randomSession();
    auto result = co_await meta.create({SUPER_USER, "file", session, O_RDWR, p644});
    CO_ASSERT_OK(result);
    co_await randomWrite(meta, storage, result->stat, folly::Random::rand64(100ULL << 30), 1ULL << 20);
    CO_ASSERT_OK(co_await meta.close({SUPER_USER, result->stat.id, session, true, std::nullopt, std::nullopt}));
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "file", AtFlags(), false}));

    co_await folly::coro::sleep(std::chrono::seconds(1));
    CO_ASSERT_OK(co_await meta.stat({SUPER_USER, result->stat.id, AtFlags()}));

    // only enable gc delay if free space > 95
    config.mock_meta().gc().set_gc_delay_free_space_threshold(95);
    // wait 1s, then inode is deleted
    co_await folly::coro::sleep(std::chrono::seconds(1));
    CO_ASSERT_INODE_NOT_EXISTS(result->stat.id);
  }());
}

TYPED_TEST(TestGcManager, recurisvePerm) {
  MockCluster::Config config;
  config.mock_meta().set_recursive_remove_perm_check(0);
  config.mock_meta().gc().set_enable(true);
  config.mock_meta().gc().set_recursive_perm_check(true);

  auto ua = flat::UserInfo(flat::Uid(1), flat::Gid(1));
  auto ub = flat::UserInfo(flat::Uid(2), flat::Gid(2));

  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();

    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "trash", p755, true}));
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "trash/gc-orphans", {}, 0, p644}));

    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "share", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ua, "share/ua", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ub, "share/ua/ub", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ub, "share/ua/ub/noperm-empty", p700, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ub, "share/ua/ub/noperm-notempty/subdir", p700, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ua, "share/ua/protected-directory", p755, true}));
    CO_ASSERT_OK(co_await meta.setAttr(
        SetAttrReq::setIFlags(SUPER_USER, "share/ua/protected-directory", IFlags(FS_IMMUTABLE_FL))));
    CO_ASSERT_OK(co_await meta.mkdirs({ua, "share/ua/normal-dir", p755, true}));
    CO_ASSERT_OK(co_await meta.create({ua, "share/ua/normal-dir/protected1", {}, 0, p644}));
    CO_ASSERT_OK(co_await meta.create({ua, "share/ua/normal-dir/protected2", {}, 0, p644}));
    CO_ASSERT_OK(co_await meta.setAttr(
        SetAttrReq::setIFlags(SUPER_USER, "share/ua/normal-dir/protected1", IFlags(FS_IMMUTABLE_FL))));
    CO_ASSERT_OK(co_await meta.setAttr(
        SetAttrReq::setIFlags(SUPER_USER, "share/ua/normal-dir/protected2", IFlags(FS_IMMUTABLE_FL))));
    CO_ASSERT_OK(co_await meta.mkdirs({ua, "share/ua/data1/noperm/subdir", p755, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ua, "share/ua/data2/noperm/subdir", p755, true}));
    CO_ASSERT_OK(co_await meta.setAttr(
        SetAttrReq::setPermission(SUPER_USER, "share/ua/data1/noperm", AtFlags(), {}, {}, flat::Permission(0))));
    CO_ASSERT_OK(co_await meta.setAttr(
        SetAttrReq::setPermission(SUPER_USER, "share/ua/data2/noperm", AtFlags(), {}, {}, flat::Permission(0))));

    CO_ASSERT_OK(co_await meta.remove({ua, "share/ua", AtFlags(), true}));
    co_await folly::coro::sleep(5_s);
    CO_ASSERT_OK(co_await printTree(meta));
  }());
}

TYPED_TEST(TestGcManager, DISABLED_BadFile) {
  // generate too many logs, disable by default
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(true);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();
    auto &storage = cluster.meta().getStorageClient();

    // inject fault on chain 1
    co_await dynamic_cast<storage::client::StorageClientInMem &>(storage).injectErrorOnChain(
        flat::ChainId(100001),
        makeError(StorageClientCode::kChecksumMismatch));

    // create file on chain 1, then remove
    for (size_t i = 0; i < 10000; i++) {
      auto layout = Layout::newChainList(4096, {flat::ChainId(100001)});
      CO_ASSERT_OK(co_await meta.create({SUPER_USER, fmt::format("{}.1", i), std::nullopt, O_RDONLY, p644, layout}));
      CO_ASSERT_OK(co_await meta.remove({SUPER_USER, fmt::format("{}.1", i), AtFlags(), false}));
    }
    // create file on chain 2. then remove
    for (size_t i = 0; i < 10000; i++) {
      auto layout = Layout::newChainList(4096, {flat::ChainId(100002)});
      CO_ASSERT_OK(co_await meta.create({SUPER_USER, fmt::format("{}.2", i), std::nullopt, O_RDONLY, p644, layout}));
      CO_ASSERT_OK(co_await meta.remove({SUPER_USER, fmt::format("{}.2", i), AtFlags(), false}));
    }

    co_await folly::coro::sleep(std::chrono::seconds(5));
    CO_ASSERT_INODE_CNTS(10010);
  }());
}

}  // namespace hf3fs::meta::server