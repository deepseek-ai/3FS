#include <fcntl.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Task.h>
#include <gtest/gtest.h>
#include <linux/fs.h>

#include "common/utils/FaultInjection.h"
#include "common/utils/StatusCode.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Service.h"
#include "meta/store/MetaStore.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {
namespace {

template <typename KV>
class TestMkdirs : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestMkdirs, KVTypes);

TYPED_TEST(TestMkdirs, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "a", p755, false}));
    CO_ASSERT_ERROR(co_await meta.mkdirs({SUPER_USER, "b/c", p755, false}), MetaCode::kNotFound);
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "b/c", p755, true}));

    for (auto path : {"a", "b", "b/c"}) {
      auto result = co_await meta.open({SUPER_USER, path, {}, O_DIRECTORY});
      CO_ASSERT_OK(result);
    }

    auto result = co_await meta.mkdirs({SUPER_USER, "b/c", p755, true});
    CO_ASSERT_ERROR(result, MetaCode::kExists);
    auto result2 = co_await meta.mkdirs({SUPER_USER, "/", p700, true});
    CO_ASSERT_ERROR(result2, MetaCode::kExists);

    flat::UserInfo otherUser(Uid(1), Gid(1), String());
    CO_ASSERT_ERROR(co_await meta.mkdirs({otherUser, "d/e", p755, true}), MetaCode::kNoPermission);

    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setIFlags(SUPER_USER, "/", IFlags(FS_CHAIN_ALLOCATION_FL))));
    auto result3 = co_await meta.stat({SUPER_USER, "/", AtFlags()});
    CO_ASSERT_OK(result3);
    CO_ASSERT_TRUE(result3->stat.acl.iflags & FS_CHAIN_ALLOCATION_FL);

    auto result4 = co_await meta.mkdirs({SUPER_USER, "f/g", p755, true});
    CO_ASSERT_OK(result4);
    CO_ASSERT_TRUE(result4->stat.acl.iflags & FS_CHAIN_ALLOCATION_FL);
    CO_ASSERT_EQ(result4->stat.asDirectory().chainAllocCounter, -1);

    auto dir = result4->stat.id;
    auto result5 = co_await meta.create(CreateReq(SUPER_USER, PathAt(dir, "a"), std::nullopt, OpenFlags(), p755));
    CO_ASSERT_OK(result5);
    CO_ASSERT_EQ(result5->stat.id.useNewChunkEngine(), cluster.config().mock_meta().enable_new_chunk_engine());
    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setIFlags(SUPER_USER, dir, IFlags(FS_NEW_CHUNK_ENGINE))));
    auto result6 = co_await meta.create(CreateReq(SUPER_USER, PathAt(dir, "b"), std::nullopt, OpenFlags(), p755));
    CO_ASSERT_OK(result6);
    CO_ASSERT_TRUE(result6->stat.id.useNewChunkEngine());
  }());
}

TYPED_TEST(TestMkdirs, ConflictSet) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &store = cluster.meta().getStore();

    for (int i = 0; i < 100; i++) {
      std::string path = std::to_string(i) + ".txt";
      CHECK_CONFLICT_SET(
          [&](auto &txn) -> CoTask<void> {
            auto req = MkdirsReq(SUPER_USER, Path(path), p755, false);
            auto createResult = co_await store.mkdirs(req)->run(txn);
            CO_ASSERT_OK(createResult);
          },
          (std::vector<String>{
              MetaTestHelper::getInodeKey(InodeId::root()),           // parent Inode
              MetaTestHelper::getDirEntryKey(InodeId::root(), path),  // dir entry
          }),
          (std::vector<String>{
              MetaTestHelper::getDirEntryKey(InodeId::root(), path),  // dir entry
          }),
          false);
    }
  }());
}

TYPED_TEST(TestMkdirs, Concurrent) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &store = cluster.meta().getStore();

    // concurrent mkdir on same path should conflict
    for (int i = 0; i < 100; i++) {
      std::string path = std::to_string(i) + ".dir";
      auto mkdir = [&](auto &txn) -> CoTask<void> {
        auto req = MkdirsReq(SUPER_USER, Path(path), p755, false);
        auto mkdirResult = co_await store.mkdirs(req)->run(txn);
        CO_ASSERT_OK(mkdirResult);
      };
      CO_ASSERT_CONFLICT(mkdir, mkdir);
    }

    // concurrent mkdir on different path shouldn't conflict
    for (int i = 0; i < 100; i++) {
      auto mkdir1 = [&](auto &txn) -> CoTask<void> {
        auto req = MkdirsReq(SUPER_USER, Path(std::to_string(i) + ".1"), p755, false);
        auto mkdirResult = co_await store.mkdirs(req)->run(txn);
        CO_ASSERT_OK(mkdirResult);
      };
      auto mkdir2 = [&](auto &txn) -> CoTask<void> {
        auto req = MkdirsReq(SUPER_USER, Path(std::to_string(i) + ".2"), p755, false);
        auto mkdirResult = co_await store.mkdirs(req)->run(txn);
        CO_ASSERT_OK(mkdirResult);
      };
      CO_ASSERT_NO_CONFLICT(mkdir1, mkdir2);
    }
  }());
}

TYPED_TEST(TestMkdirs, FaultInjection) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    auto result = co_await meta.mkdirs({SUPER_USER, "dirA/dirB/dirC", p755, true});
    CO_ASSERT_OK(result);
    std::vector<Path> parents =
        {"/", "/dirA", "/dirA/dirB", "/dirA/dirB/dirC", "/dirA/dirB/../", "/dirA/dirB/../dirB", "not_exists"};

    for (auto i = 0; i < 100; i++) {
      FAULT_INJECTION_SET(5, 3);  // 5%, 3 faults.
      auto parent = parents[folly::Random::rand32(parents.size())];
      auto path = parent;
      path.append(std::to_string(i));
      auto result = co_await meta.mkdirs({SUPER_USER, path, p755, false});
      if (parent == "not_exists") {
        CO_ASSERT_ERROR(result, MetaCode::kNotFound);
      } else {
        CO_ASSERT_OK(result);
      }
    }
  }());
}

}  // namespace
}  // namespace hf3fs::meta::server
