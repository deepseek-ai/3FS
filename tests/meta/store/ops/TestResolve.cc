#include <bits/types/FILE.h>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Task.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <iterator>
#include <variant>
#include <vector>

#include "common/utils/Coroutine.h"
#include "common/utils/StatusCode.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "meta/components/AclCache.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/PathResolve.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

template <typename KV>
class TestResolve : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestResolve, KVTypes);

TYPED_TEST(TestResolve, ResolveComponent) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    AclCache aclCache(1 << 20);

    READ_ONLY_TRANSACTION({
      // when resolve with inexistent dentry, do not return error
      DirEntry parentEntry = DirEntry::newDirectory(MetaTestHelper::randomInodeId(),
                                                    "not-exists-dir",
                                                    MetaTestHelper::randomInodeId(),
                                                    rootp777);
      auto resolveResult = co_await PathResolveOp(*txn, aclCache, SUPER_USER).pathComponent(parentEntry, "not-exists");
      CO_ASSERT_OK(resolveResult);

      // parentId other than rootId will trigger inode load
      auto parentId = MetaTestHelper::randomInodeId();
      resolveResult = co_await PathResolveOp(*txn, aclCache, SUPER_USER).pathComponent(parentId, "not-exists");
      CO_ASSERT_ERROR(resolveResult, MetaCode::kNotFound);
    });

    {  // if parent is not directory, should return kNotDirectory
      Inode parentInode = Inode::newFile(MetaTestHelper::randomInodeId(),
                                         rootp644,
                                         MetaTestHelper::randomLayout(),
                                         UtcClock::now().castGranularity(1_s));
      READ_WRITE_TRANSACTION_OK({
        auto storeInodeResult = co_await parentInode.store(*txn);
        CO_ASSERT_OK(storeInodeResult);
      });
      READ_ONLY_TRANSACTION({
        auto resolveResult =
            co_await PathResolveOp(*txn, aclCache, SUPER_USER).pathComponent(parentInode.id, "not-exists");
        CO_ASSERT_ERROR(resolveResult, MetaCode::kNotDirectory);
      });
    }

    {
      // lookup not exist path, if user doesn't have permission, should return kNoPermission, otherwise return
      // kNotFound.
      Inode parentInode = Inode::newDirectory(MetaTestHelper::randomInodeId(),
                                              InodeId::root(),
                                              "parent",
                                              rootp700,
                                              Layout(),
                                              UtcClock::now().castGranularity(1_s));
      DirEntry parentEntry = DirEntry::newDirectory(InodeId::root(), "dir-name", parentInode.id, rootp700);
      CO_ASSERT_TRUE(parentInode.isDirectory());
      READ_WRITE_TRANSACTION_OK({
        // create a directory
        auto storeInodeResult = co_await parentInode.store(*txn);
        CO_ASSERT_OK(storeInodeResult);
      });
      READ_ONLY_TRANSACTION({
        // check permission
        flat::UserInfo otherUser(Uid(1), Gid(1), String());
        auto resolveResult =
            co_await PathResolveOp(*txn, aclCache, otherUser).pathComponent(parentInode.id, "not-exists");
        CO_ASSERT_ERROR(resolveResult, MetaCode::kNoPermission);

        resolveResult = co_await PathResolveOp(*txn, aclCache, otherUser).pathComponent(parentEntry, "not-exists");
        CO_ASSERT_ERROR(resolveResult, MetaCode::kNoPermission);

        // should return parent info if parent exists but dirEntry not exists
        // pass in parentId other than rootId will trigger inode load
        resolveResult = co_await PathResolveOp(*txn, aclCache, SUPER_USER).pathComponent(parentInode.id, "not-exists");
        CO_ASSERT_OK(resolveResult);
        CO_ASSERT_EQ(resolveResult->getParentId(), parentInode.id);
        CO_ASSERT_EQ(resolveResult->getParentAcl(), parentInode.acl);

        resolveResult = co_await PathResolveOp(*txn, aclCache, SUPER_USER).pathComponent(parentEntry, "not-exists");
        CO_ASSERT_OK(resolveResult);
        CO_ASSERT_TRUE(!std::holds_alternative<Inode>(resolveResult->parent));
        CO_ASSERT_EQ(resolveResult->getParentId(), parentInode.id);
      });
    }

    {
      // create a directory and a entry
      Inode parentInode = Inode::newDirectory(MetaTestHelper::randomInodeId(),
                                              InodeId::root(),
                                              "parent",
                                              rootp700,
                                              Layout(),
                                              UtcClock::now().castGranularity(1_s));
      DirEntry childEntry = DirEntry::newFile(parentInode.id, "file", MetaTestHelper::randomInodeId());
      CO_ASSERT_TRUE(parentInode.isDirectory());
      READ_WRITE_TRANSACTION_OK({
        auto storeInodeResult = co_await parentInode.store(*txn);
        auto storeEntryResult = co_await childEntry.store(*txn);
        CO_ASSERT_OK(storeInodeResult);
        CO_ASSERT_OK(storeEntryResult);
      });
      READ_ONLY_TRANSACTION({
        auto resolveResult = co_await PathResolveOp(*txn, aclCache, SUPER_USER).pathComponent(parentInode.id, "file");
        CO_ASSERT_OK(resolveResult);
        CO_ASSERT_TRUE(std::holds_alternative<Inode>(resolveResult->parent));
        CO_ASSERT_EQ(std::get<Inode>(resolveResult->parent), parentInode);
        CO_ASSERT_TRUE(resolveResult->dirEntry.has_value());
        CO_ASSERT_EQ(resolveResult->dirEntry.value(), childEntry);
      });
    }
  }());
}

TYPED_TEST(TestResolve, pathRange) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    AclCache aclCache(1 << 20);
    READ_ONLY_TRANSACTION({
      // if parent doesn't exists, do not return error
      // if path is empty, return kNotFound
      auto parentId = MetaTestHelper::randomInodeId();
      auto path = PathAt(parentId, "/a/b/c");
      auto result1 = co_await PathResolveOp(*txn, aclCache, SUPER_USER).pathRange(path);
      CO_ASSERT_OK(result1);

      path = PathAt(parentId, "a/b/c");
      auto result2 = co_await PathResolveOp(*txn, aclCache, SUPER_USER).pathRange(path);
      CO_ASSERT_ERROR(result2, MetaCode::kNotFound);
    });

    // create some directory file and link
    auto ts = UtcClock::now().castGranularity(1_s);
    auto root = Inode::newDirectory(InodeId::root(), InodeId::root(), "/", rootp755, Layout(), ts);
    auto a =
        Inode::newDirectory(MetaTestHelper::randomInodeId(), InodeId::root(), "a", rootp700, Layout(), ts);  // dir: /a
    auto aEntry = DirEntry::newDirectory(InodeId::root(), "a", a.id, rootp700);
    auto b = Inode::newDirectory(MetaTestHelper::randomInodeId(), a.id, "b", rootp700, Layout(), ts);  // dir: /a/b
    auto bEntry = DirEntry::newDirectory(a.id, "b", b.id, rootp700);
    auto c = Inode::newDirectory(MetaTestHelper::randomInodeId(), b.id, "c", rootp700, Layout(), ts);  // dir: /a/b/c
    auto cEntry = DirEntry::newDirectory(b.id, "c", c.id, rootp700);
    auto d = Inode::newFile(MetaTestHelper::randomInodeId(),
                            rootp644,
                            MetaTestHelper::randomLayout(),
                            ts);  // file: /a/b/c/d
    auto dEntry = DirEntry::newFile(c.id, "d", d.id);
    auto e = Inode::newSymlink(MetaTestHelper::randomInodeId(),
                               "../c",
                               rootUid,
                               rootGid,
                               ts);  // symlink: /a/b/c/e -> '../c'
    auto eEntry = DirEntry::newSymlink(c.id, "e", e.id);
    auto f = Inode::newSymlink(MetaTestHelper::randomInodeId(), "d", rootUid, rootGid, ts);  // symlink: /a/b/c/f -> 'd'
    auto fEntry = DirEntry::newSymlink(c.id, "f", f.id);
    auto g = Inode::newSymlink(MetaTestHelper::randomInodeId(),
                               "g",
                               rootUid,
                               rootGid,
                               ts);  // symlink: /a/b/c/g -> 'g' (loop)
    auto gEntry = DirEntry::newSymlink(c.id, "g", g.id);
    std::vector<Inode> inodes = {root, a, b, c, d, e, f, g};
    std::vector<DirEntry> entries = {aEntry, bEntry, cEntry, dEntry, eEntry, fEntry, gEntry};
    READ_WRITE_TRANSACTION_OK({
      for (auto &inode : inodes) {
        CO_ASSERT_OK(co_await inode.store(*txn));
      }
      for (auto &entry : entries) {
        CO_ASSERT_OK(co_await entry.store(*txn));
      }
    });

    READ_ONLY_TRANSACTION({
      // resolve: /a/b/c/d -> d
      auto path = PathAt("/a/b/c/d");
      Path trace;
      auto result = co_await PathResolveOp(*txn, aclCache, SUPER_USER, &trace).pathRange(path);
      std::cout << "resolve " << *path.path << " -> " << trace << " " << trace.lexically_normal() << std::endl;
      CO_ASSERT_OK(result);
      CO_ASSERT_TRUE(result->missing.empty()) << result->missing;
      CO_ASSERT_EQ(result->getParentId(), c.id);
      CO_ASSERT_EQ(result->dirEntry.value(), dEntry);
    });

    READ_ONLY_TRANSACTION({
      // resolve: /a/b/c/e -> e
      auto path = Path("/a/b/c/e");
      Path trace;
      auto result = co_await PathResolveOp(*txn, aclCache, SUPER_USER, &trace).pathRange(path);
      std::cout << "resolve " << path << " -> " << trace << " " << trace.lexically_normal() << std::endl;
      CO_ASSERT_OK(result);
      CO_ASSERT_TRUE(result->missing.empty()) << result->missing;
      CO_ASSERT_EQ(result->getParentId(), c.id);
      CO_ASSERT_EQ(result->dirEntry.value(), eEntry);
    });

    READ_ONLY_TRANSACTION({
      // resolve: /a/b/c/e/d -> should get d
      auto path = PathAt("/a/b/c/e/d");
      Path trace;
      auto result = co_await PathResolveOp(*txn, aclCache, SUPER_USER, &trace).pathRange(path);
      CO_ASSERT_OK(result);
      std::cout << "resolve " << *path.path << " -> " << trace << " " << trace.lexically_normal() << std::endl;
      CO_ASSERT_TRUE(result->missing.empty()) << result->missing;
      CO_ASSERT_EQ(result->getParentId(), c.id);
      CO_ASSERT_EQ(result->dirEntry.value(), dEntry);
    });

    READ_ONLY_TRANSACTION({
      // resolve: /a/b/c/f/xyz -> /a/b/c/f is symlink point to file, should get kNotDirectory
      auto path = PathAt("/a/b/c/f/xyz");
      Path trace;
      auto result = co_await PathResolveOp(*txn, aclCache, SUPER_USER, &trace).pathRange(path);
      CO_ASSERT_ERROR(result, MetaCode::kNotDirectory);
    });

    READ_ONLY_TRANSACTION({
      // resolve: /a/b/c/g/xyz, /a/b/c/g is a symlink that cause loop, should get kTooManySymlinks
      auto path = PathAt("a/b/c/g/xyz");  // should still work
      Path trace;
      auto result = co_await PathResolveOp(*txn, aclCache, SUPER_USER, &trace).pathRange(path);
      CO_ASSERT_ERROR(result, MetaCode::kTooManySymlinks);
    });

    READ_ONLY_TRANSACTION({
      // resolve /a/x/y/z -> stop at x
      auto path = PathAt("/a/x/y/z");
      auto result = co_await PathResolveOp(*txn, aclCache, SUPER_USER).pathRange(path);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->missing, "x/y/z");
    });
  }());
}

TYPED_TEST(TestResolve, path) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    AclCache aclCache(1 << 20);

    // some path /a/b/c here
    auto ts = UtcClock::now().castGranularity(1_s);
    auto root = Inode::newDirectory(InodeId::root(), InodeId::root(), "/", rootp755, Layout(), ts);
    auto a =
        Inode::newDirectory(MetaTestHelper::randomInodeId(), InodeId::root(), "a", rootp700, Layout(), ts);  // dir: /a
    auto aEntry = DirEntry::newDirectory(InodeId::root(), "a", a.id, rootp700);
    auto b = Inode::newDirectory(MetaTestHelper::randomInodeId(), a.id, "b", rootp700, Layout(), ts);  // dir: /a/b
    auto bEntry = DirEntry::newDirectory(a.id, "b", b.id, rootp700);
    auto c = Inode::newDirectory(MetaTestHelper::randomInodeId(), b.id, "c", rootp700, Layout(), ts);  // dir: /a/b/c
    auto cEntry = DirEntry::newDirectory(b.id, "c", c.id, rootp700);
    std::vector<Inode> inodes = {root, a, b, c};
    std::vector<DirEntry> entries = {aEntry, bEntry, cEntry};
    READ_WRITE_TRANSACTION_OK({
      for (auto &inode : inodes) {
        auto result = co_await inode.store(*txn);
        CO_ASSERT_OK(result);
      }
      for (auto &entry : entries) {
        auto result = co_await entry.store(*txn);
        CO_ASSERT_OK(result);
      }
    });

    // todo: should add more test for follow symlink

    READ_ONLY_TRANSACTION({
      // resolve /a/b/c
      auto result =
          co_await PathResolveOp(*txn, aclCache, SUPER_USER).path(Path("/a/b/c"), AtFlags(AT_SYMLINK_NOFOLLOW));
      CO_ASSERT_OK(result);
      CO_ASSERT_TRUE(!std::holds_alternative<Inode>(result->parent));
      CO_ASSERT_EQ(result->getParentId(), b.id);
      CO_ASSERT_TRUE(result->dirEntry.has_value());
      CO_ASSERT_EQ(result->dirEntry.value(), cEntry);

      // resolve /a/b/../b/c
      result =
          co_await PathResolveOp(*txn, aclCache, SUPER_USER).path(Path("/a/b/../b/c"), AtFlags(AT_SYMLINK_NOFOLLOW));
      CO_ASSERT_OK(result);
      CO_ASSERT_TRUE(!std::holds_alternative<Inode>(result->parent));
      CO_ASSERT_EQ(result->getParentId(), b.id);
      CO_ASSERT_TRUE(result->dirEntry.has_value());
      CO_ASSERT_EQ(result->dirEntry.value(), cEntry);

      // resolve /a/b/c/../c
      result =
          co_await PathResolveOp(*txn, aclCache, SUPER_USER).path(Path("/a/b/../b/c"), AtFlags(AT_SYMLINK_NOFOLLOW));
      CO_ASSERT_OK(result);
      CO_ASSERT_TRUE(!std::holds_alternative<Inode>(result->parent));
      CO_ASSERT_EQ(result->getParentId(), b.id);
      CO_ASSERT_TRUE(result->dirEntry.has_value());
      CO_ASSERT_EQ(result->dirEntry.value(), cEntry);
    });

    READ_ONLY_TRANSACTION({
      // resolve /a/d -> parent found but dirEntry not found
      auto result = co_await PathResolveOp(*txn, aclCache, SUPER_USER).path(Path("/a/d"), AtFlags(AT_SYMLINK_NOFOLLOW));
      CO_ASSERT_OK(result);
      CO_ASSERT_TRUE(!std::holds_alternative<Inode>(result->parent));
      CO_ASSERT_EQ(result->getParentId(), a.id);
      CO_ASSERT_FALSE(result->dirEntry.has_value());

      // resolve /a/../a/d
      result = co_await PathResolveOp(*txn, aclCache, SUPER_USER).path(Path("/a/d"), AtFlags(AT_SYMLINK_NOFOLLOW));
      CO_ASSERT_OK(result);
      CO_ASSERT_TRUE(!std::holds_alternative<Inode>(result->parent));
      CO_ASSERT_EQ(result->getParentId(), a.id);
      CO_ASSERT_FALSE(result->dirEntry.has_value());
    });

    READ_ONLY_TRANSACTION({
      // resolve /a/d/e -> kNotFound
      auto result =
          co_await PathResolveOp(*txn, aclCache, SUPER_USER).path(Path("/a/d/e"), AtFlags(AT_SYMLINK_NOFOLLOW));
      CO_ASSERT_ERROR(result, MetaCode::kNotFound);

      // resolve /a/d/../d/e
      result = co_await PathResolveOp(*txn, aclCache, SUPER_USER).path(Path("/a/d/e"), AtFlags(AT_SYMLINK_NOFOLLOW));
      CO_ASSERT_ERROR(result, MetaCode::kNotFound);
    });
  }());
}

}  // namespace hf3fs::meta::server
