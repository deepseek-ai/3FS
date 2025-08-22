#include <cstdlib>
#include <fcntl.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Task.h>
#include <functional>
#include <gtest/gtest.h>
#include <optional>
#include <string>

#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "gtest/gtest.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/ops/BatchOperation.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {
namespace {

template <typename KV>
class TestRename : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestRename, KVTypes);

TYPED_TEST(TestRename, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    FAULT_INJECTION_SET(10, 5);

    // create a, rename a -> b, stat a -> InodeId::root(), stat b -> Inode
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "a", {}, O_RDONLY, p644}));
    CO_ASSERT_OK(co_await meta.rename({SUPER_USER, "a", "b"}));
    auto a = co_await meta.stat({SUPER_USER, "a", AtFlags(AT_SYMLINK_FOLLOW)});
    CO_ASSERT_ERROR(a, MetaCode::kNotFound);
    auto b = co_await meta.stat({SUPER_USER, "b", AtFlags(AT_SYMLINK_FOLLOW)});
    CO_ASSERT_OK(b);

    // create file c, d, rename c -> d, c will replace d
    InodeId cId;
    auto c = co_await meta.create({SUPER_USER, "c", {}, O_RDONLY, p644});
    auto d = co_await meta.create({SUPER_USER, "d", {}, O_RDONLY, p644});
    CO_ASSERT_OK(c);
    CO_ASSERT_OK(d);
    cId = c->stat.id;

    // check rename return inode
    auto renameResult = co_await meta.rename({SUPER_USER, "c", "d"});
    CO_ASSERT_OK(renameResult);
    CO_ASSERT_TRUE(renameResult->stat.has_value());
    CO_ASSERT_EQ(renameResult->stat->id, cId);
    auto statResult = co_await meta.stat({SUPER_USER, "d", AtFlags(AT_SYMLINK_FOLLOW)});
    CO_ASSERT_OK(statResult);
    CO_ASSERT_EQ(statResult->stat, renameResult->stat);

    auto c1 = co_await meta.stat({SUPER_USER, "c", AtFlags(AT_SYMLINK_FOLLOW)});
    CO_ASSERT_ERROR(c1, MetaCode::kNotFound);
    auto d1 = co_await meta.stat({SUPER_USER, "d", AtFlags(AT_SYMLINK_FOLLOW)});
    CO_ASSERT_OK(d1);
    CO_ASSERT_EQ(d1->stat.id, cId);

    // create directory e, f/g, rename e -> f, should return not empty
    auto f = co_await meta.mkdirs({SUPER_USER, "e", p755, false});
    auto g = co_await meta.mkdirs({SUPER_USER, "f/g", p755, true});
    CO_ASSERT_OK(f);
    CO_ASSERT_OK(g);

    CO_ASSERT_ERROR(co_await meta.rename({SUPER_USER, "e", "f"}), MetaCode::kNotEmpty);

    // remove f/g
    FAULT_INJECTION_SET(0, 0);
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "f/g", AtFlags(), false}));

    // rename e -> f again
    FAULT_INJECTION_SET(10, 5);
    CO_ASSERT_OK(co_await meta.rename({SUPER_USER, "e", "f"}));
  }());
}

TYPED_TEST(TestRename, Directory) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "dir1", p777, false}));
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "dir2", p777, false}));
    auto stat1 = (co_await meta.stat({SUPER_USER, "dir1", AtFlags()}))->stat;
    CO_ASSERT_EQ(stat1.asDirectory().name, "dir1");
    CO_ASSERT_OK(co_await meta.rename({SUPER_USER, "dir1", "dir2"}));
    auto stat2 = (co_await meta.stat({SUPER_USER, "dir2", AtFlags()}))->stat;
    CO_ASSERT_EQ(stat2.asDirectory().name, "dir2");
    CO_ASSERT_EQ(stat2.id, stat1.id);
  }());
}

TYPED_TEST(TestRename, Trash) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(true);
    config.mock_meta().gc().set_gc_directory_delay(0_s);
    config.mock_meta().gc().set_gc_file_delay(0_s);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();

    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "dir", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "trash", p777, false}));
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "dir/a", {}, O_RDONLY, p644}));
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "dir/b", {}, O_RDONLY, p644}));
    CO_ASSERT_OK(co_await meta.rename({SUPER_USER, "dir/a", "trash/trash_name", true}));
    CO_ASSERT_ERROR(co_await meta.rename({SUPER_USER, "dir/b", "trash/trash_name", true}), MetaCode::kExists);
  }());
}

TYPED_TEST(TestRename, GC) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(true);
    config.mock_meta().gc().set_gc_directory_delay(0_s);
    config.mock_meta().gc().set_gc_file_delay(0_s);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();

    // rename directory to trash, should be owner and rwx permission
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "trash", p777, true}));
    auto ua = flat::UserInfo(flat::Uid(1), flat::Gid(1));
    auto ub = flat::UserInfo(flat::Uid(2), flat::Gid(2));
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "ua", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ua, "ua/data-500", flat::Permission(0500), true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ua, "ua/data-777", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({ua, "trash/ua", p777, true}));
    CO_ASSERT_ERROR(co_await meta.rename({ua, "ua/data-777", "trash/data-777"}), MetaCode::kNoPermission);
    CO_ASSERT_OK(co_await meta.rename({ua, "ua/data-777", "trash/data-777", true}));
    CO_ASSERT_ERROR(co_await meta.rename({ua, "ua/data-500", "trash/data-500", true}), MetaCode::kNoPermission);
    CO_ASSERT_OK(co_await meta.rename({ua, "trash/ua", "trash/ua-dest"}));

    CO_ASSERT_OK(co_await meta.mkdirs({ua, "ua/dir_has_root", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "ua/dir_has_root/root", p700, true}));
    CO_ASSERT_ERROR(co_await meta.rename({ua, "ua/dir_has_root", "trash/dir_has_root", true}), MetaCode::kNoPermission);

    // rename directory to another directory which is under GC, should fail
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "dir1/subdir", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "dir2", p777, false}));
    auto stat = (co_await meta.stat({SUPER_USER, "dir1", AtFlags()}))->stat;
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "dir1", AtFlags(), true}));
    CO_ASSERT_ERROR(co_await meta.rename({SUPER_USER, "dir2", PathAt(stat.id, "dir")}), MetaCode::kNotFound);
    CO_ASSERT_ERROR(co_await meta.rename({SUPER_USER, "dir2", PathAt(InodeId::gcRoot(), "orphan")}),
                    MetaCode::kNoPermission);

    // rename replace a file, the file should be removed by GC.
    InodeId oldDstId;
    // create src and dst
    auto createSrc = co_await meta.create({SUPER_USER, "a", {}, O_RDONLY, p644});
    auto createDst = co_await meta.create({SUPER_USER, "b", {}, O_RDONLY, p644});
    CO_ASSERT_OK(createSrc);
    CO_ASSERT_OK(createDst);
    oldDstId = createDst->stat.id;

    // do rename to replace old dst
    CO_ASSERT_OK(co_await meta.rename({SUPER_USER, "a", "b"}));
    // wait GC tasks
    std::this_thread::sleep_for(std::chrono::seconds(1));
    // GC should remove old dst
    CO_ASSERT_INODE_NOT_EXISTS(oldDstId);
  }());
}

TYPED_TEST(TestRename, ConflictSet) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();
    auto &store = cluster.meta().getStore();

    for (int i = 0; i < 100; i++) {
      std::string src = std::to_string(i) + ".src";
      std::string dst = std::to_string(i) + ".dst";
      InodeId srcId;
      // create src first
      auto result = co_await meta.mkdirs({SUPER_USER, src, p755, false});
      CO_ASSERT_OK(result);
      srcId = result->stat.id;

      CHECK_CONFLICT_SET(
          [&](auto &txn) -> CoTask<void> {
            auto req = RenameReq(SUPER_USER, Path(src), Path(dst));
            auto removeResult = co_await store.rename(req)->run(txn);
            CO_ASSERT_OK(removeResult);
          },
          (std::vector<String>{
              MetaTestHelper::getDirEntryKey(InodeId::root(), src),  // src dirEntry
              MetaTestHelper::getDirEntryKey(InodeId::root(), dst),  // dst dirEntry
              MetaTestHelper::getInodeKey(InodeId::root()),          // dst parent Inode
              MetaTestHelper::getInodeKey(srcId),                    // src Inode
          }),
          (std::vector<String>{
              MetaTestHelper::getDirEntryKey(InodeId::root(), src),  // src dirEntry
              MetaTestHelper::getDirEntryKey(InodeId::root(), dst),  // dst dirEntry
              MetaTestHelper::getInodeKey(srcId),                    // src Inode
          }),
          false);
    }
  }());
}

TYPED_TEST(TestRename, Concurrent) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.set_num_meta(1);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();
    auto &store = cluster.meta().getStore();

    auto rename = [&](Path src, Path dst) -> std::function<CoTask<void>(IReadWriteTransaction &)> {
      return [=, &store](auto &txn) -> CoTask<void> {  // rename src to dst
        auto req = RenameReq(SUPER_USER, Path(src), Path(dst));
        auto renameResult = co_await store.rename(req)->run(txn);
        CO_ASSERT_OK(renameResult);
      };
    };
    auto create =
        [&](InodeId parent, Path path, bool directory) -> std::function<CoTask<void>(IReadWriteTransaction &)> {
      EXPECT_FALSE(path.has_parent_path());
      return [=, &store](auto &txn) -> CoTask<void> {  // create or mkdir at dst
        if (directory) {
          auto req = MkdirsReq(SUPER_USER, PathAt(parent, path), p755, true);
          auto mkdirResult = co_await store.mkdirs(req)->run(txn);
          CO_ASSERT_OK(mkdirResult);
        } else {
          auto req = CreateReq(SUPER_USER, PathAt(parent, path), {}, O_RDONLY, p644);
          CO_ASSERT_OK(co_await BatchedOp::create(store, txn, req));
        }
      };
    };

    // rename src -> dst and concurrent create dst should conflict
    for (int i = 0; i < 100; i++) {
      std::string src = std::to_string(i) + ".src-1";
      std::string dst = std::to_string(i) + ".dst-1";
      // create src first
      CO_ASSERT_OK(co_await meta.create({SUPER_USER, src, {}, O_RDONLY, p644}));
      if (i % 2 == 0) {
        CO_ASSERT_CONFLICT(rename(src, dst), create(InodeId::root(), dst, i % 2 == 0));
      } else {
        CO_ASSERT_CONFLICT(create(InodeId::root(), dst, i % 2 == 0), rename(src, dst));
      }
    }

    // rename to same destination should conflict
    for (int i = 0; i < 100; i++) {
      std::string src1 = std::to_string(i) + ".src-2.1";
      std::string src2 = std::to_string(i) + ".src-2.2";
      std::string dst = std::to_string(i) + ".dst-2";
      for (auto path : {src1, src2}) {
        CO_ASSERT_OK(co_await meta.create({SUPER_USER, path, {}, O_RDONLY, p644}));
      }
      auto rename1 = rename(src1, dst);  // rename src1 to dst
      auto rename2 = rename(src2, dst);  // rename src2 to dst
      if (i % 2 == 0) {
        CO_ASSERT_CONFLICT(rename1, rename2);
      } else {
        CO_ASSERT_CONFLICT(rename2, rename1);
      }
    }

    // rename src to different dst should be conflict
    for (int i = 0; i < 100; i++) {
      std::string src = std::to_string(i) + ".src-3";
      std::string dst1 = std::to_string(i) + ".dst-3.1";
      std::string dst2 = std::to_string(i) + ".dst-3.2";
      // create src1, src2 first
      CO_ASSERT_OK(co_await meta.create({SUPER_USER, Path(src), {}, O_RDONLY, p644}));
      auto rename1 = rename(src, dst1);  // rename src to dst1
      auto rename2 = rename(src, dst2);  // rename src to dst2
      if (i % 2 == 0) {
        CO_ASSERT_CONFLICT(rename1, rename2);
      } else {
        CO_ASSERT_CONFLICT(rename2, rename1);
      }
    }

    // rename different srcs to different dsts shouldn't be conflict
    for (int i = 0; i < 100; i++) {
      std::string src1 = std::to_string(i) + ".src-4.1";
      std::string src2 = std::to_string(i) + ".src-4.2";
      std::string dst1 = std::to_string(i) + ".dst-4.1";
      std::string dst2 = std::to_string(i) + ".dst-4.2";
      for (auto path : {src1, src2}) {
        CO_ASSERT_OK(co_await meta.create({SUPER_USER, path, {}, O_RDONLY, p644}));
      }
      auto rename1 = rename(src1, dst1);  // rename src1 to dst1
      auto rename2 = rename(src2, dst2);  // rename src2 to dst2
      CO_ASSERT_NO_CONFLICT(rename1, rename2);
    }

    // rename replace a empty directory and create under directory should be conflict.
    for (int i = 0; i < 100; i++) {
      std::string src = std::to_string(i) + ".src-5";
      std::string dst = std::to_string(i) + ".dst-5";
      InodeId oldDstId;
      // create src and dst first
      auto result = co_await meta.mkdirs({SUPER_USER, src, p755, false});
      CO_ASSERT_OK(result);
      result = co_await meta.mkdirs({SUPER_USER, dst, p755, false});
      CO_ASSERT_OK(result);
      oldDstId = result->stat.id;
      if (i % 2 == 0) {
        CO_ASSERT_CONFLICT(rename(src, dst), create(oldDstId, "child", (i % 2 == 0)));
      } else {
        CO_ASSERT_CONFLICT(create(oldDstId, "child", (i % 2 == 0)), rename(src, dst));
      }
    }

    // mkdir /a /b/d, rename /a -> /b/d/e and rename /b/d -> /a/c should conflict
    // mkdir /a, /b/d/e
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "a", p755, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "b/d", p755, true}));
    if (folly::Random::oneIn(2)) {
      CO_ASSERT_CONFLICT(rename("/a", "/b/d/e"), rename("/b/d", "/a/c"));
    } else {
      CO_ASSERT_CONFLICT(rename("/b/d", "/a/c"), rename("/a", "/b/d/e"));
    }
  }());
}

}  // namespace
}  // namespace hf3fs::meta::server
