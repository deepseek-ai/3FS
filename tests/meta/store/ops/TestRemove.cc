#include <chrono>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/Task.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/UtcTime.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Service.h"
#include "gtest/gtest.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Utils.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

template <typename KV>
class TestRemove : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestRemove, KVTypes);

TYPED_TEST(TestRemove, GC) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(false);
    config.mock_meta().gc().set_gc_directory_delay(0_s);
    config.mock_meta().gc().set_gc_file_delay(0_s);
    config.mock_meta().gc().set_scan_interval(10_ms);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();
    auto &gcManager = cluster.meta().getGcManager();

    int fileCnt = 512;

    // create a directory, create some files, subdirectories, symlinks under it, then remove it recursively
    auto mkdirResult = co_await meta.mkdirs({SUPER_USER, "directory", p755, false});
    CO_ASSERT_OK(mkdirResult);
    auto &directory = mkdirResult->stat;
    for (int i = 0; i < fileCnt; i++) {
      CO_ASSERT_OK(co_await meta.create({SUPER_USER, PathAt(directory.id, std::to_string(i) + ".file"), {}, 0, p644}));
      CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, PathAt(directory.id, std::to_string(i) + ".dir"), p755, false}));
      CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, PathAt(directory.id, std::to_string(i) + ".symlink"), "target"}));
    }

    GET_INODE_CNTS(inodes);
    GET_DIRENTRY_CNTS(entries);
    fmt::print("inodes {} dirEntries {}\n", inodes, entries);
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "directory", AtFlags(), true}));

    GET_INODE_CNTS(numInodes);

    config.mock_meta().gc().set_enable(true);
    std::this_thread::sleep_for(std::chrono::seconds(4));

    READ_WRITE_TRANSACTION_NO_COMMIT({
      for (auto gcDir : gcManager.currGcDirectories()) {
        auto empty = co_await DirEntryList::checkEmpty(*txn, gcDir->dirId());
        CO_ASSERT_OK(empty);
        CO_ASSERT_TRUE(empty.value());
      }

      // all inodes except root, gcRoot, gcDirectory should be removed
      CO_ASSERT_INODE_CNTS(numInodes - 1 - fileCnt * 3);
    });
  }());
}

TYPED_TEST(TestRemove, Remove) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(false);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();

    FAULT_INJECTION_SET(10, 3);

    CO_ASSERT_ERROR(co_await meta.remove({SUPER_USER, "not-exists", AtFlags(), false}), MetaCode::kNotFound);
    CO_ASSERT_ERROR(co_await meta.remove({SUPER_USER, "/", AtFlags(), false}), StatusCode::kInvalidArg);
    CO_ASSERT_ERROR(co_await meta.remove({SUPER_USER, "not-exists/", AtFlags(), false}), StatusCode::kInvalidArg);
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "a/b", p755, true}));

    // remove a not empty directory
    CO_ASSERT_ERROR(co_await meta.remove({SUPER_USER, "a", AtFlags(), false}), MetaCode::kNotEmpty);
    // remove a not empty directory recursively
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "a", AtFlags(), true}));
    // stat removed directory
    CO_ASSERT_ERROR(co_await meta.stat({SUPER_USER, "a", AtFlags(AT_SYMLINK_NOFOLLOW)}), MetaCode::kNotFound);

    auto result = co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p644});
    CO_ASSERT_OK(result);
    auto &inode = result->stat;
    CO_ASSERT_OK(co_await meta.hardLink({SUPER_USER, "file", "file-hardlink", AtFlags(AT_SYMLINK_NOFOLLOW)}));
    auto statResult = co_await meta.stat({SUPER_USER, "file-hardlink", AtFlags(AT_SYMLINK_NOFOLLOW)});
    CO_ASSERT_OK(statResult);
    CO_ASSERT_EQ(inode.nlink, 1);
    CO_ASSERT_EQ(statResult->stat.nlink, 2);
    CO_ASSERT_EQ(inode.id, statResult->stat.id);

    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "file", AtFlags(), false}));
    statResult = co_await meta.stat({SUPER_USER, "file-hardlink", AtFlags(AT_SYMLINK_NOFOLLOW)});
    CO_ASSERT_OK(statResult);
    CO_ASSERT_EQ(statResult->stat.nlink, 1);
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "file-hardlink", AtFlags(), false}));
    CO_ASSERT_ERROR(co_await meta.stat({SUPER_USER, "file-hardlink", AtFlags(AT_SYMLINK_NOFOLLOW)}),
                    MetaCode::kNotFound);
  }());
}

TYPED_TEST(TestRemove, RemoveRecursivePerm) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();
    auto ua = flat::UserInfo(flat::Uid(1), flat::Gid(1));
    auto ub = flat::UserInfo(flat::Uid(2), flat::Gid(2));

    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "shared", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ua, "shared/ua/subdir", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ua, "shared/ua2/subdir", flat::Permission(0222), true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ua, "shared/ua_ub/subdir", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ub, "shared/ua_ub/subdir/dir", p700, false}));
    CO_ASSERT_OK(co_await meta.mkdirs({ua, "shared/ua_ub2/subdir", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ub, "shared/ua_ub2/subdir/dir", flat::Permission(0777 & S_ISVTX), false}));

    FAULT_INJECTION_SET(10, 3);
    CO_ASSERT_ERROR(co_await meta.remove({ua, "shared/ua2", AtFlags(), true}), MetaCode::kNoPermission);
    CO_ASSERT_ERROR(co_await meta.remove({ub, "shared/ua", AtFlags(), true}), MetaCode::kNoPermission);
    CO_ASSERT_OK(co_await meta.remove({ua, "shared/ua", AtFlags(), true}));
    CO_ASSERT_ERROR(co_await meta.remove({ua, "shared/ua_ub", AtFlags(), true}), MetaCode::kNoPermission);
    CO_ASSERT_ERROR(co_await meta.remove({ua, "shared/ua_ub2", AtFlags(), true}), MetaCode::kNoPermission);
  }());
}

TYPED_TEST(TestRemove, RemoveRecursive) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(false);
    config.mock_meta().gc().set_gc_directory_delay(0_s);
    config.mock_meta().gc().set_gc_file_delay(0_s);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();

    // remove a non-empty directory recursively should remove subdirectory too.
    auto mkdirResult = co_await meta.mkdirs({SUPER_USER, "dir", p755, true});
    CO_ASSERT_OK(mkdirResult);
    auto dirId = mkdirResult->stat.id;
    // create a sub directory
    auto subDirResult = co_await meta.mkdirs({SUPER_USER, "dir/subdir", p755, true});
    CO_ASSERT_OK(subDirResult);
    auto subDirId = subDirResult->stat.id;
    // create a file under directory
    auto createResult = co_await meta.create({SUPER_USER, PathAt(subDirId, "file"), {}, O_EXCL, p644});
    CO_ASSERT_OK(createResult);
    auto fileId = createResult->stat.id;

    for (auto path : {"dir/.", "dir/..", "dir/subdir", "dir/subdir/file"}) {
      fmt::print("path {}\n", path);
      CO_ASSERT_OK(co_await meta.stat({SUPER_USER, PathAt(path), AtFlags(AT_SYMLINK_FOLLOW)}));
    }

    // remove directory
    auto removeResult = co_await meta.remove({SUPER_USER, "dir", AtFlags(), true});
    CO_ASSERT_OK(removeResult);
    READ_ONLY_TRANSACTION({
      // all inodes present
      for (auto inodeId : {dirId, subDirId, fileId}) {
        CO_ASSERT_OK((co_await Inode::snapshotLoad(*txn, inodeId)).then(checkMetaFound<Inode>));
      }
      // dir entry should present
      for (auto [parent, name] : {std::pair(dirId, "subdir"), std::pair(subDirId, "file")}) {
        fmt::print("parent {}, name {}\n", parent, name);
        CO_ASSERT_OK((co_await DirEntry::snapshotLoad(*txn, parent, name)).then(checkMetaFound<DirEntry>));
      }
    });

    // lookup at deleted directory
    for (auto [parent, name] : {std::pair{dirId, "subdir"}, {subDirId, "file"}}) {
      fmt::print("parent {}, name {}\n", parent, name);
      CO_ASSERT_OK(co_await meta.stat({SUPER_USER, PathAt(parent, name), AtFlags(AT_SYMLINK_FOLLOW)}));
    }
    for (auto path : {"dir/.", "dir/..", "dir/subdir", "dir/subdir/file"}) {
      fmt::print("path {}\n", path);
      CO_ASSERT_ERROR(co_await meta.stat({SUPER_USER, PathAt(path), AtFlags(AT_SYMLINK_FOLLOW)}), MetaCode::kNotFound);
    }
    CO_ASSERT_ERROR(co_await meta.create({SUPER_USER, PathAt(dirId, "file2"), {}, O_EXCL, p644}), MetaCode::kNotFound);
    auto file2 = co_await meta.create({SUPER_USER, PathAt(subDirId, "file2"), {}, O_EXCL, p644});
    CO_ASSERT_OK(file2);

    config.mock_meta().gc().set_enable(true);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    READ_ONLY_TRANSACTION({
      // all inodes removed
      for (auto inodeId : {dirId, subDirId, fileId, file2->stat.id}) {
        CO_ASSERT_ERROR((co_await Inode::snapshotLoad(*txn, inodeId)).then(checkMetaFound<Inode>), MetaCode::kNotFound);
      }
    });
    // after GC, directory has deleted, create under directory should get kNotFound
    CO_ASSERT_ERROR(co_await meta.create({SUPER_USER, PathAt(dirId, "another-file"), {}, O_EXCL, p644}),
                    MetaCode::kNotFound);
    CO_ASSERT_ERROR(co_await meta.create({SUPER_USER, PathAt(subDirId, "another-file"), {}, O_EXCL, p644}),
                    MetaCode::kNotFound);
    // lookup at deleted directory should get kNotFound
    for (auto pair : {std::pair{dirId, "."}, {dirId, ".."}, {dirId, "subdir"}, {subDirId, "file"}}) {
      CO_ASSERT_ERROR(co_await meta.stat({SUPER_USER, PathAt(pair.first, pair.second), AtFlags(AT_SYMLINK_FOLLOW)}),
                      MetaCode::kNotFound);
    }
  }());
}

TYPED_TEST(TestRemove, RemoveDirectoryByInode) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(false);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();

    FAULT_INJECTION_SET(10, 3);

    // remove a non-empty directory recursively should remove subdirectory too.
    auto mkdirResult = co_await meta.mkdirs({SUPER_USER, "dir", p755, true});
    CO_ASSERT_OK(mkdirResult);
    auto dirId = mkdirResult->stat.id;
    // create a sub directory
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "dir/subdir", p755, true}));
    auto createResult = co_await meta.create({SUPER_USER, PathAt(dirId, "file"), {}, O_EXCL, p644});
    CO_ASSERT_OK(createResult);
    auto fileId = createResult->stat.id;

    CO_ASSERT_ERROR(co_await meta.remove({SUPER_USER, fileId, AtFlags(), false}), MetaCode::kNotDirectory);
    // remove directory
    CO_ASSERT_ERROR(co_await meta.remove({SUPER_USER, dirId, AtFlags(), false}), MetaCode::kNotEmpty);
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, dirId, AtFlags(), true}));
  }());
}

TYPED_TEST(TestRemove, Symlink) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(false);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();

    CO_ASSERT_OK(co_await meta.create({SUPER_USER, PathAt("file"), {}, O_EXCL, p644}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, PathAt("symlink1"), "file"}));
    CO_ASSERT_OK(co_await meta.stat({SUPER_USER, PathAt("symlink1"), AtFlags(AT_SYMLINK_FOLLOW)}));
    CO_ASSERT_OK(
        co_await meta.hardLink({SUPER_USER, PathAt("symlink1"), PathAt("symlink2"), AtFlags(AT_SYMLINK_NOFOLLOW)}));
    CO_ASSERT_OK(co_await meta.stat({SUPER_USER, PathAt("symlink2"), AtFlags(AT_SYMLINK_FOLLOW)}));
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, PathAt("symlink1"), AtFlags(AT_SYMLINK_NOFOLLOW), false}));
    CO_ASSERT_OK(co_await meta.stat({SUPER_USER, PathAt("symlink2"), AtFlags(AT_SYMLINK_FOLLOW)}));
  }());
}

TYPED_TEST(TestRemove, ConflictSet) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(false);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();
    auto &store = cluster.meta().getStore();

    // remove file
    for (int i = 0; i < 100; i++) {
      std::string path = std::to_string(i) + ".file";
      auto result = co_await meta.create({SUPER_USER, path, {}, 0, p644});
      CO_ASSERT_OK(result);
      auto inodeId = result->stat.id;
      CHECK_CONFLICT_SET(
          [&](auto &txn) -> CoTask<void> {
            auto req = RemoveReq(SUPER_USER, Path(path), AtFlags(0), false);
            auto removeResult = co_await store.remove(req)->run(txn);
            CO_ASSERT_OK(removeResult);
          },
          (std::vector<String>{
              MetaTestHelper::getDirEntryKey(InodeId::root(), path),
              MetaTestHelper::getInodeKey(inodeId),
          }),
          (std::vector<String>{
              MetaTestHelper::getInodeKey(inodeId),
          }),
          false);
    }

    // remove directory
    for (int i = 0; i < 1; i++) {
      std::string path = std::to_string(i) + ".directory";
      auto result = co_await meta.mkdirs({SUPER_USER, path, p755, false});
      CO_ASSERT_OK(result);
      auto inodeId = result->stat.id;
      CHECK_CONFLICT_SET(
          [&](auto &txn) -> CoTask<void> {
            auto req = RemoveReq(SUPER_USER, Path(path), AtFlags(0), false);
            auto removeResult = co_await store.remove(req)->run(txn);
            CO_ASSERT_OK(removeResult);
          },
          (std::vector<String>{
              MetaTestHelper::getDirEntryKey(InodeId::root(), path),
          }),
          (std::vector<String>{
              MetaTestHelper::getInodeKey(inodeId),  // src Inode
          }),
          false);
    }
  }());
}

TYPED_TEST(TestRemove, Idempotent) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(false);
    config.mock_meta().set_idempotent_record_expire(5_s);
    config.mock_meta().set_idempotent_record_clean(1_s);
    config.set_num_meta(1);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();

    auto remove1 = RemoveReq({SUPER_USER, PathAt("file"), AtFlags(AT_SYMLINK_NOFOLLOW), false});
    auto remove2 = RemoveReq({SUPER_USER, PathAt("file"), AtFlags(AT_SYMLINK_NOFOLLOW), false});
    CO_ASSERT_ERROR(co_await meta.remove(remove1), MetaCode::kNotFound);
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, PathAt("file"), {}, O_EXCL, p644}));
    CO_ASSERT_ERROR(co_await meta.remove(remove1), MetaCode::kNotFound);
    CO_ASSERT_OK(co_await meta.remove(remove2));
    CO_ASSERT_OK(co_await meta.remove(remove2));
    CO_ASSERT_ERROR(co_await meta.remove(remove1), MetaCode::kNotFound);
    co_await folly::coro::sleep(2_s);
    CO_ASSERT_OK(co_await meta.remove(remove2));
    co_await folly::coro::sleep(5_s);
    CO_ASSERT_ERROR(co_await meta.remove(remove2), MetaCode::kNotFound);
    // remove check inode id
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, PathAt("file"), {}, O_EXCL, p644}));
    CO_ASSERT_ERROR(
        co_await meta.remove(
            {SUPER_USER, PathAt("file"), AtFlags(AT_SYMLINK_NOFOLLOW), false, false, InodeId(folly::Random::rand64())}),
        MetaCode::kNotFound);
    co_return;
  }());
}

TYPED_TEST(TestRemove, ConcurrentCreate) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(false);
    config.set_num_meta(1);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();
    auto &store = cluster.meta().getStore();

    // rmdir and concurrent create should conflict
    for (int i = 0; i < 100; i++) {
      auto dirPath = std::to_string(i) + ".directory";
      auto childPath = "child";
      auto mkdir = co_await meta.mkdirs({SUPER_USER, dirPath, p755, true});
      CO_ASSERT_OK(mkdir);
      auto parentId = mkdir->stat.id;
      auto remove = [&](auto &txn) -> CoTask<void> {
        auto req = RemoveReq(SUPER_USER, Path(dirPath), AtFlags(0), false);
        auto removeResult = co_await store.remove(req)->run(txn);
        CO_ASSERT_OK(removeResult);
      };
      auto create = [&](auto &txn) -> CoTask<void> {
        if (i % 2 == 0) {
          auto req = MkdirsReq(SUPER_USER, PathAt(parentId, childPath), p755, false);
          auto mkdirResult = co_await store.mkdirs(req)->run(txn);
          CO_ASSERT_OK(mkdirResult);
        } else {
          auto req = CreateReq(SUPER_USER, PathAt(parentId, childPath), {}, O_RDONLY, p644);
          auto createResult = co_await BatchedOp::create(store, txn, req);
          CO_ASSERT_OK(createResult);
        }
      };
      if (i % 2 == 0) {
        CO_ASSERT_CONFLICT(remove, create);
      } else {
        CO_ASSERT_CONFLICT(create, remove);
      }
    }
  }());
}

}  // namespace hf3fs::meta::server
