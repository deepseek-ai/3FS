#include <array>
#include <fcntl.h>
#include <folly/Random.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Task.h>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/kv/mem/MemKV.h"
#include "common/kv/mem/MemKVEngine.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Path.h"
#include "common/utils/RandomUtils.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "fdb/FDB.h"
#include "fmt/core.h"
#include "gtest/gtest.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/ops/BatchOperation.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {
namespace {

template <typename KV>
class TestCreate : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestCreate, KVTypes);

TYPED_TEST(TestCreate, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config cfg;
    cfg.mock_meta().set_check_file_hole(true);
    auto cluster = this->createMockCluster(cfg);
    auto &meta = cluster.meta().getOperator();

    // create "file", should success
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p644}));

    // create "file" again should success
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p644}));

    // create file and open write without session
    CO_ASSERT_ERROR(co_await meta.create({SUPER_USER, "file2", {}, O_RDWR, p644}), StatusCode::kInvalidArg);

    // create "file" again with or without O_EXCL
    CO_ASSERT_ERROR(co_await meta.create({SUPER_USER, "file", {}, O_EXCL, p644}), MetaCode::kExists);
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p644}));

    // other have no create permission, but can open files that already exists.
    flat::UserInfo otherUser(Uid(1), Gid(1), String());
    CO_ASSERT_OK(co_await meta.create({otherUser, "file", {}, O_RDONLY, p644}));
    CO_ASSERT_ERROR(co_await meta.create({otherUser, "file2", {}, O_RDONLY, p644}), MetaCode::kNoPermission);

    // create under a directory that not exist
    CO_ASSERT_ERROR(co_await meta.create({SUPER_USER, "directory/file", {}, O_RDONLY, p644}), MetaCode::kNotFound);
    CO_ASSERT_ERROR(
        co_await meta.create({SUPER_USER, PathAt(MetaTestHelper::randomInodeId(), "file"), {}, O_RDONLY, p644}),
        MetaCode::kNotFound);

    CO_ASSERT_ERROR(co_await meta.create({SUPER_USER, "/", {}, O_RDONLY, p644}), StatusCode::kInvalidArg);
    CO_ASSERT_ERROR(co_await meta.create({SUPER_USER, "a/", {}, O_RDONLY, p644}), StatusCode::kInvalidArg);
    CO_ASSERT_ERROR(co_await meta.create({SUPER_USER, "/a/b/.", {}, O_RDONLY, p644}), StatusCode::kInvalidArg);
    CO_ASSERT_ERROR(co_await meta.create({SUPER_USER, "/a/..", {}, O_RDONLY, p644}), StatusCode::kInvalidArg);

    // create under directory
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "directory/sub-directory", p755, true}));
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "directory/file", {}, O_EXCL, p644}));
    auto oresult = co_await meta.open({SUPER_USER, "directory", {}, O_DIRECTORY});
    CO_ASSERT_OK(oresult);
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, PathAt(oresult->stat.id, "file2"), {}, O_EXCL, p644}));

    CO_ASSERT_ERROR(co_await meta.create({SUPER_USER, "directory/sub-directory", {}, O_RDONLY, p644}),
                    MetaCode::kIsDirectory);

    READ_ONLY_TRANSACTION({
      auto result = co_await DirEntryList::snapshotLoad(*txn, InodeId::root(), "", 4096);
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->entries.size(), 2);  // file, directory
      for (size_t i = 0; i < result->entries.size(); i++) {
        fmt::print("{}/{} -> {}\n", result->entries.at(i).parent, result->entries.at(i).name, result->entries.at(i).id);
      }
    });
  }());
}

TYPED_TEST(TestCreate, TestSetGid) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    auto shareGroup = Gid(12345);
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "shared", Permission(S_ISGID | 0777), false}));
    CO_ASSERT_OK(co_await meta.setAttr(
        SetAttrReq::setPermission(SUPER_USER, "shared", AtFlags(), std::nullopt, shareGroup, std::nullopt)));
    auto dir = co_await meta.mkdirs({SUPER_USER, "shared/subdir1/subdir2", Permission(0070), true});
    CO_ASSERT_OK(dir);
    CO_ASSERT_TRUE(dir->stat.acl.perm & S_ISGID) << fmt::format("{:o}", dir->stat.acl.uid);

    auto file = co_await meta.create({SUPER_USER, "shared/subdir1/subdir2/file", {}, O_RDONLY, Permission(0640)});
    CO_ASSERT_OK(file);
    CO_ASSERT_EQ(file->stat.acl.gid, shareGroup);
    CO_ASSERT_FALSE(file->stat.acl.perm & S_ISGID);

    CO_ASSERT_OK(
        co_await meta.open({flat::UserInfo{Uid(1), shareGroup, ""}, "shared/subdir1/subdir2/file", {}, O_RDONLY}));
  }());
}

TYPED_TEST(TestCreate, ConflictSet) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.set_num_meta(1);
    auto cluster = this->createMockCluster(config);
    auto &store = cluster.meta().getStore();
    auto &meta = cluster.meta().getOperator();

    {
      auto path = "create-new-file";
      auto session = MetaTestHelper::randomSession();
      InodeId inodeId;
      CHECK_CONFLICT_SET(
          [&](auto &txn) -> CoTask<void> {
            auto req = CreateReq(SUPER_USER, Path(path), session, O_RDWR, p644);
            auto createResult = co_await BatchedOp::create(store, txn, req);
            CO_ASSERT_OK(createResult);
            inodeId = createResult->stat.id;
          },
          (std::vector<String>{
              MetaTestHelper::getInodeKey(InodeId::root()),           // parent Inode
              MetaTestHelper::getDirEntryKey(InodeId::root(), path),  // dir entry
              MetaTestHelper::getInodeKey(inodeId),
              std::string(kv::kMetadataVersionKey),
          }),
          (std::vector<String>{
              MetaTestHelper::getDirEntryKey(InodeId::root(), path),  // dir entry
              MetaTestHelper::getInodeKey(inodeId),
              MetaTestHelper::getInodeSessionKey(FileSession::create(inodeId, session)),
              // MetaTestHelper::getClientSessionKey(FileSession::create(inodeId, session)),
              // todo: if per directory chain allocation enabled, should contains parent
              // MetaTestHelper::getInodeKey(InodeId::root())
          }),
          true);
    }

    {
      auto path = "create-trunc-exists-file";
      auto result = co_await meta.create({SUPER_USER, path, {}, O_RDONLY, p644});
      CO_ASSERT_OK(result);
      auto session = MetaTestHelper::randomSession();
      InodeId inodeId;
      CHECK_CONFLICT_SET(
          [&](auto &txn) -> CoTask<void> {
            auto req = CreateReq(SUPER_USER, Path(path), session, O_TRUNC | O_WRONLY, p644);
            auto createResult = co_await BatchedOp::create(store, txn, req);
            CO_ASSERT_OK(createResult);
            inodeId = createResult->stat.id;
          },
          (std::vector<String>{std::string(kv::kMetadataVersionKey)}),
          (std::vector<String>{
              MetaTestHelper::getInodeSessionKey(FileSession::create(inodeId, session)),
              // MetaTestHelper::getClientSessionKey(FileSession::create(inodeId, session))
          }),
          true);
    }

    // don't allow replace exists file on create
    // {
    //   auto path = "create-trunc-replace-exists-file";
    //   auto result = co_await meta.create({SUPER_USER, path, {}, O_RDONLY, p644});
    //   CO_ASSERT_OK(result);
    //   auto oldInodeId = result->stat.id;
    //   CO_ASSERT_OK(co_await meta.sync(
    //       {SUPER_USER, oldInodeId, true, std::nullopt, std::nullopt, false, VersionedLength{2ULL << 30, 0}}));
    //   auto session = MetaTestHelper::randomSession();
    //   InodeId newInodeId;
    //   CHECK_CONFLICT_SET(
    //       [&](auto &txn) -> CoTask<void> {
    //         auto req = CreateReq(SUPER_USER, Path(path), session, O_TRUNC | O_WRONLY, p644);
    //         auto createResult = co_await BatchedOp::create(store,  txn, req);
    //         CO_ASSERT_OK(createResult);
    //         newInodeId = createResult->stat.id;
    //         CO_ASSERT_NE(oldInodeId, newInodeId);
    //       },
    //       (std::vector<String>{MetaTestHelper::getDirEntryKey(InodeId::root(), path),
    //                            MetaTestHelper::getInodeKey(oldInodeId)}),
    //       (std::vector<String>{MetaTestHelper::getDirEntryKey(InodeId::root(), path),
    //                            MetaTestHelper::getInodeKey(newInodeId),
    //                            MetaTestHelper::getInodeSessionKey(FileSession::create(newInodeId, session)),
    //                            MetaTestHelper::getClientSessionKey(FileSession::create(newInodeId, session))}),
    //       false);
    // }

    {
      auto path = "create-open-exists-file";
      auto result = co_await meta.create({SUPER_USER, path, {}, O_RDONLY | O_CREAT, p644});
      CO_ASSERT_OK(result);
      CHECK_CONFLICT_SET(
          [&](auto &txn) -> CoTask<void> {
            auto req = CreateReq(SUPER_USER, Path(path), {}, O_RDONLY | O_CREAT, p644);
            CO_ASSERT_OK(co_await BatchedOp::create(store, txn, req));
          },
          (std::vector<String>{std::string(kv::kMetadataVersionKey)}),
          (std::vector<String>{}),
          true);
    }
  }());
}

TYPED_TEST(TestCreate, Concurrent) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.set_num_meta(1);
    auto cluster = this->createMockCluster(config);
    auto &store = cluster.meta().getStore();

    // concurrent create on same path should conflict
    for (int i = 0; i < 100; i++) {
      auto path = std::to_string(i) + ".txt";
      auto createTask = [&](auto &txn) -> CoTask<void> {
        auto req = CreateReq(SUPER_USER, Path(path), {}, O_RDONLY, p644);
        auto createResult = co_await BatchedOp::create(store, txn, req);
        CO_ASSERT_OK(createResult);
      };
      CO_ASSERT_CONFLICT(createTask, createTask);
    }

    // concurrent create a different path is conflict because per directory chain allocation counter.
    // concurrent create on different path shouldn't conflict
    // for (int i = 0; i < 100; i++) {
    //   auto create1 = [&](auto &txn) -> CoTask<void> {
    //     auto req = CreateReq(SUPER_USER, Path(std::to_string(i) + ".1"), {}, O_RDONLY, p644);
    //     auto createResult = co_await BatchedOp::create(store, txn, req);
    //     CO_ASSERT_OK(createResult);
    //   };
    //   auto create2 = [&](auto &txn) -> CoTask<void> {
    //     auto req = CreateReq(SUPER_USER, Path(std::to_string(i) + ".2"), {}, O_RDONLY, p644);
    //     auto createResult = co_await BatchedOp::create(store, txn, req);
    //     CO_ASSERT_OK(createResult);
    //   };
    //   CO_ASSERT_NO_CONFLICT(create1, create2);
    // }
  }());
}

TYPED_TEST(TestCreate, FaultInjection) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    auto result = co_await meta.mkdirs({SUPER_USER, "dirA/dirB/dirC", p755, true});
    CO_ASSERT_OK(result);
    std::vector<Path> parents =
        {"/", "/dirA", "/dirA/dirB", "/dirA/dirB/dirC", "/dirA/dirB/../", "/dirA/dirB/../dirB", "not_exists"};

    for (auto i = 0; i < 100; i++) {
      FAULT_INJECTION_SET(10, 3);  // 10%, 3 faults.
      auto parent = parents[folly::Random::rand32(parents.size())];
      auto path = parent;
      path.append(std::to_string(i));
      auto result = co_await meta.create({SUPER_USER, path, {}, O_RDONLY, p644});
      if (parent == "not_exists") {
        CO_ASSERT_ERROR(result, MetaCode::kNotFound);
      } else {
        CO_ASSERT_OK(result);
      }
    }
  }());
}

TYPED_TEST(TestCreate, OTrunc) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config cfg;
    cfg.mock_meta().set_check_file_hole(true);
    auto cluster = this->createMockCluster(cfg);
    auto &meta = cluster.meta().getOperator();

    for (size_t i = 0; i < 10; i++) {
      auto fname = fmt::format("truncate-inplace-{}", i);
      auto create = co_await meta.create({SUPER_USER, fname, {}, O_EXCL, p644});
      CO_ASSERT_OK(create);
      CO_ASSERT_OK(co_await meta.sync(
          {SUPER_USER, create->stat.id, true, std::nullopt, std::nullopt, false, VersionedLength{1_MB, 0}}));

      auto createAgain = co_await meta.create({SUPER_USER, fname, MetaTestHelper::randomSession(), O_RDWR, p644});
      CO_ASSERT_OK(createAgain);
      CO_ASSERT_FALSE(createAgain->needTruncate);

      auto session = MetaTestHelper::randomSession();
      auto open = co_await meta.create({SUPER_USER, fname, session, O_TRUNC | O_RDWR, p644});
      CO_ASSERT_OK(open);
      CO_ASSERT_EQ(open->stat.id, create->stat.id);
      CO_ASSERT_TRUE(open->needTruncate);
      CO_ASSERT_NE(open->stat.asFile().length, 0);
    }
  }());
}

TYPED_TEST(TestCreate, EventLog) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    for (size_t i = 0; i < 10; i++) {
      FAULT_INJECTION_SET(10, 2);
      auto create = co_await meta.create({SUPER_USER, fmt::format("{}", i), {}, O_EXCL, p644});
      CO_ASSERT_OK(create);
    }

    for (size_t i = 0; i < 10; i++) {
      // create again, shouldn't have log.
      FAULT_INJECTION_SET(10, 2);
      auto create = co_await meta.create({SUPER_USER, fmt::format("{}", i), {}, O_RDONLY, p644});
      CO_ASSERT_OK(create);
    }

    // todo: how to verify log counts here?
  }());
}

TYPED_TEST(TestCreate, batch) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.set_num_meta(1);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();
    auto &store = cluster.meta().getStore();
    auto &engine = *this->kvEngine();

    for (size_t i = 0; i < 10; i++) {
      auto dir1 = (co_await meta.mkdirs({SUPER_USER, fmt::format("{}.1", i), p755, false}))->stat.id;
      auto dir2 = (co_await meta.mkdirs({SUPER_USER, fmt::format("{}.2", i), p755, false}))->stat.id;
      std::vector<std::pair<CreateReq, Result<CreateRsp>>> reqs;
      auto cnts = folly::Random::rand32(200);
      for (auto i = 0u; i < cnts; i++) {
        auto path = folly::Random::rand32(10);
        auto user = folly::Random::oneIn(2) ? SUPER_USER : flat::UserInfo{Uid(1), Gid(1)};
        auto perm = folly::Random::rand32(0777);
        auto flags = folly::Random::rand32(O_RDWR + 1);
        for (auto flag : {O_CREAT, O_TRUNC, O_EXCL, O_DIRECTORY}) {
          if (folly::Random::oneIn(2)) {
            flags |= flag;
          }
        }
        FAULT_INJECTION_SET(5, 3);
        CreateReq req{user,
                      PathAt(dir1, folly::to<std::string>(path)),
                      flags != O_RDONLY ? MetaTestHelper::randomSession() : std::optional<SessionInfo>(),
                      OpenFlags(flags),
                      Permission(perm)};
        auto rsp = co_await meta.create(req);
        reqs.push_back({req, rsp});
      }

      std::vector<std::unique_ptr<BatchedOp::Waiter<CreateReq, CreateRsp>>> waiters;
      auto batch = std::make_unique<BatchedOp>(store, dir2);
      for (auto &[req, rsp] : reqs) {
        auto req1 = req;
        req1.path.parent = dir2;
        auto waiter = std::make_unique<BatchedOp::Waiter<CreateReq, CreateRsp>>(req1);
        batch->add(*waiter);
        waiters.emplace_back(std::move(waiter));
      }

      // Note: test can't pass with fault injection enabled, result may change if inject MaybeCommtted error.
      // FAULT_INJECTION_SET(5, 3);
      auto txn = engine.createReadWriteTransaction();
      auto driver = OperationDriver(*batch, Void{});
      CO_ASSERT_OK(co_await driver.run(std::move(txn), {}, false, false));

      for (auto i = 0u; i < reqs.size(); i++) {
        auto req = reqs[i].first;
        auto expected = reqs[i].second;
        auto result = waiters[i]->getResult();
        if (expected.hasError()) {
          CO_ASSERT_TRUE(result.hasError())
              << fmt::format("req {} expected {}, result {}", req, expected.error(), result.value());
          CO_ASSERT_EQ(result.error().code(), expected.error().code());
          CO_ASSERT_NE(result.error().code(), MetaCode::kFoundBug);
        } else {
          CO_ASSERT_FALSE(result.hasError())
              << fmt::format("req {} expected {}, result {}", req, expected.value(), result.error());
          CO_ASSERT_EQ(expected->stat.acl, result->stat.acl);
          CO_ASSERT_EQ(expected->needTruncate, result->needTruncate);
          CO_ASSERT_FALSE((expected->needTruncate && !req.flags.contains(O_TRUNC)));
        }
      }
    }
  }());
}

}  // namespace
}  // namespace hf3fs::meta::server
