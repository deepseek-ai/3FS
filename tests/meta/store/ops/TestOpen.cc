#include <algorithm>
#include <fcntl.h>
#include <folly/Random.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/Task.h>
#include <gtest/gtest.h>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "common/utils/FaultInjection.h"
#include "common/utils/StatusCode.h"
#include "fmt/core.h"
#include "gtest/gtest.h"
#include "meta/store/MetaStore.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {
namespace {

template <typename KV>
class TestOpen : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestOpen, KVTypes);

TYPED_TEST(TestOpen, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();

    // create file, directory and symlink
    CO_ASSERT_OK(
        co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p644, std::nullopt, true /* dynamic stripe */}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "symlink", "file"}));
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "dir/dir2", p700, true}));

    // open file and symlink should success, and both point to file
    auto result1 = co_await meta.open({SUPER_USER, "file", {}, O_RDONLY});
    auto result2 = co_await meta.open({SUPER_USER, "symlink", {}, O_RDONLY});
    CO_ASSERT_OK(result1);
    CO_ASSERT_OK(result2);
    CO_ASSERT_EQ(result1->stat, result2->stat);

    // open with InodeId
    auto result3 = co_await meta.open({SUPER_USER, result1->stat.id, {}, O_RDONLY});
    CO_ASSERT_OK(result3);
    CO_ASSERT_EQ(result1->stat, result3->stat);
    CO_ASSERT_NE(result3->stat.asFile().dynStripe, 0);

    auto rw1 = co_await meta.open(
        {SUPER_USER, result1->stat.id, MetaTestHelper::randomSession(), O_RDWR, true /* dynamic stripe */});
    CO_ASSERT_OK(rw1);
    CO_ASSERT_NE(rw1->stat.asFile().dynStripe, 0);

    auto rw2 = co_await meta.open(
        {SUPER_USER, result1->stat.id, MetaTestHelper::randomSession(), O_RDWR, false /* dynamic stripe */});
    CO_ASSERT_OK(rw2);
    CO_ASSERT_EQ(rw2->stat.asFile().dynStripe, 0);

    // open a not exists file, should return MetaCode::kNotFound
    CO_ASSERT_ERROR(co_await meta.open({SUPER_USER, "not-exist", {}, O_RDONLY}), MetaCode::kNotFound);
    CO_ASSERT_ERROR(co_await meta.open({SUPER_USER, "not-exist/not-exist", {}, O_RDONLY}), MetaCode::kNotFound);

    flat::UserInfo otherUser(Uid(1), Gid(1), String());
    auto session = MetaTestHelper::randomSession();
    // other user should can only open file in read only mode
    CO_ASSERT_OK(co_await meta.open({otherUser, "file", {}, O_RDONLY}));
    CO_ASSERT_ERROR(co_await meta.open({otherUser, "file", session, O_RDWR}), MetaCode::kNoPermission);

    // open not exists file under directory, su should get kNotFound, other user should get kNoPermission
    CO_ASSERT_ERROR(co_await meta.open({SUPER_USER, "dir/file", {}, O_RDONLY}), MetaCode::kNotFound);
    CO_ASSERT_ERROR(co_await meta.open({otherUser, "dir/file", {}, O_RDONLY}), MetaCode::kNoPermission);

    // open file with O_DIRECTORY should get kNotDirectory, write open directory should get kIsDirectory
    CO_ASSERT_ERROR(co_await meta.open({SUPER_USER, "file", {}, O_RDONLY | O_DIRECTORY}), MetaCode::kNotDirectory);
    CO_ASSERT_OK(co_await meta.open({SUPER_USER, "dir/dir2", {}, O_RDONLY | O_DIRECTORY}));

    CO_ASSERT_ERROR(co_await meta.open({SUPER_USER, "dir/dir2", session, O_WRONLY}), MetaCode::kIsDirectory);

    // open without session
    CO_ASSERT_ERROR(co_await meta.open({SUPER_USER, "dir/dir2", {}, O_WRONLY}), StatusCode::kInvalidArg);
    CO_ASSERT_ERROR(co_await meta.open({SUPER_USER, "dir/dir2", {}, O_RDWR}), StatusCode::kInvalidArg);

    config.mock_meta().set_readonly(true);
    CO_ASSERT_OK(co_await meta.open({otherUser, "file", {}, O_RDONLY}));
    CO_ASSERT_ERROR(co_await meta.open({SUPER_USER, "file", session, O_WRONLY}), StatusCode::kReadOnlyMode);
  }());
}

TYPED_TEST(TestOpen, Concurrent) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();
    auto &store = cluster.meta().getStore();

    auto path = "open-file-with-write";
    auto result = co_await meta.create({SUPER_USER, path, {}, O_RDONLY, p644});
    CO_ASSERT_OK(result);
    InodeId inodeId = result->stat.id;

    fmt::print("{}\n", result->stat);

    auto session = MetaTestHelper::randomSession();
    CHECK_CONFLICT_SET(
        [&](auto &txn) -> CoTask<void> {
          auto req = OpenReq(SUPER_USER, Path(path), session, O_RDWR);
          CO_ASSERT_OK(co_await store.open(req)->run(txn));
        },
        (std::vector<String>{}),
        (std::vector<String>{
            MetaTestHelper::getInodeSessionKey(FileSession::create(inodeId, session)),
            // MetaTestHelper::getClientSessionKey(FileSession::create(inodeId, session)),
        }),
        true);
  }());
}

TYPED_TEST(TestOpen, FaultInjection) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    std::vector<std::string> paths;
    for (auto i = 0; i < 10; i++) {
      std::string path = std::to_string(i);
      auto result = co_await meta.create({SUPER_USER, path, {}, O_RDONLY, p644});
      CO_ASSERT_OK(result);
      paths.push_back(path);
    }

    paths.push_back("not_exists");
    for (auto i = 0; i < 100; i++) {
      FAULT_INJECTION_SET(10, 3);  // 10%, 3 faults
      auto path = paths[folly::Random::rand32(paths.size())];
      auto result = co_await meta.open({SUPER_USER, path, {}, O_RDONLY});
      if (path == "not_exists") {
        CO_ASSERT_ERROR(result, MetaCode::kNotFound);
      } else {
        CO_ASSERT_OK(result);
      }
    }
  }());
}

// TYPED_TEST(TestOpen, PruneSession) {
//   folly::coro::blockingWait([&]() -> CoTask<void> {
//     MockCluster::Config cfg;
//     cfg.mock_meta().set_check_file_hole(true);
//     auto cluster = this->createMockCluster(cfg);
//     auto &meta = cluster.meta().getOperator();
//     auto &storage = cluster.meta().getStorageClient();

//     std::vector<std::string> path;
//     Uuid clientId = Uuid::random();
//     std::vector<Uuid> sessions;
//     std::vector<std::pair<InodeId, uint64_t>> inodes;
//     for (auto i = 0; i < 20; i++) {
//       std::string path = std::to_string(i);
//       auto sessionId = Uuid::random();
//       auto result = co_await meta.create({SUPER_USER, path, SessionInfo(ClientId(clientId), sessionId), O_RDWR,
//       p644}); CO_ASSERT_OK(result);

//       // write to make a hole
//       auto offset = folly::Random::rand32(1 << 20, 4 << 20);
//       auto length = folly::Random::rand32(2 << 20) + 1;
//       co_await randomWrite(meta, storage, result->stat, offset, length);
//       sessions.push_back(sessionId);
//       inodes.push_back({result->stat.id, offset + length});
//     }
//     for (auto i = 0; i < 100; i++) {
//       sessions.push_back(Uuid::random());
//     }
//     std::shuffle(sessions.begin(), sessions.end(), std::mt19937());
//     CO_ASSERT_OK(co_await meta.pruneSession({ClientId(clientId), sessions}));

//     co_await folly::coro::sleep(std::chrono::seconds(1));
//     for (auto [inodeId, length] : inodes) {
//       auto result = co_await meta.stat({SUPER_USER, inodeId, AtFlags(AT_SYMLINK_FOLLOW)});
//       CO_ASSERT_OK(result);
//       auto stat = result->stat;
//       CO_ASSERT_EQ(stat.asFile().length, length);
//       CO_ASSERT_TRUE(stat.asFile().hasHole());
//     }
//   }());
// }

TYPED_TEST(TestOpen, OTrunc) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config cfg;
    cfg.mock_meta().set_check_file_hole(true);
    auto cluster = this->createMockCluster(cfg);
    auto &meta = cluster.meta().getOperator();
    auto &storage = cluster.meta().getStorageClient();

    for (size_t i = 0; i < 10; i++) {
      auto fname = fmt::format("truncate-inplace-{}", i);
      auto create = co_await meta.create({SUPER_USER, fname, {}, O_EXCL, p644});
      CO_ASSERT_OK(create);
      co_await truncate(meta, storage, create->stat, 1_MB);

      FAULT_INJECTION_SET(20, 3);
      auto session = MetaTestHelper::randomSession();
      auto open = co_await meta.open({SUPER_USER, fname, session, O_TRUNC | O_RDWR});
      CO_ASSERT_EQ(open->stat.id, create->stat.id);
      CO_ASSERT_TRUE(open->needTruncate);
      CO_ASSERT_NE(open->stat.asFile().length, 0);
    }

    for (size_t i = 0; i < 10; i++) {
      auto fname = fmt::format("truncate-by-inode-{}", i);
      auto create = co_await meta.create({SUPER_USER, fname, {}, O_EXCL, p644});
      CO_ASSERT_OK(create);
      co_await truncate(meta, storage, create->stat, 2_GB);

      FAULT_INJECTION_SET(20, 3);
      auto session = MetaTestHelper::randomSession();
      auto open = co_await meta.open({SUPER_USER, create->stat.id, session, O_TRUNC | O_RDWR});
      CO_ASSERT_EQ(open->stat.id, create->stat.id);
      CO_ASSERT_TRUE(open->needTruncate);
      CO_ASSERT_NE(open->stat.asFile().length, 0);
    }

    for (size_t i = 0; i < 10; i++) {
      auto fname = fmt::format("truncate-replace-{}", i);
      auto create = co_await meta.create({SUPER_USER, fname, {}, O_EXCL, p644});
      CO_ASSERT_OK(create);
      CO_ASSERT_EQ(create->stat.asFile().dynStripe, 0);
      co_await truncate(meta, storage, create->stat, 2_GB);

      auto session1 = MetaTestHelper::randomSession();
      auto session2 = MetaTestHelper::randomSession();
      auto session3 = MetaTestHelper::randomSession();
      auto open1 = co_await meta.open({SUPER_USER, fname, session1, O_TRUNC | O_RDWR, true /* dynamic stripe */});
      CO_ASSERT_NE(open1->stat.id, create->stat.id);
      CO_ASSERT_NE(open1->stat.asFile().dynStripe, 0);

      auto open2 = co_await meta.open({SUPER_USER, fname, session2, O_TRUNC | O_RDWR});
      CO_ASSERT_EQ(open1->stat.id, open2->stat.id);

      auto result = co_await meta.stat({SUPER_USER, open1->stat.id, AtFlags(AT_SYMLINK_FOLLOW)});
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->stat.asFile().length, 0);

      co_await truncate(meta, storage, open1->stat, 2_GB);

      CO_ASSERT_OK(co_await meta.close({SUPER_USER, open1->stat.id, session1, true, {}, {}}));
      CO_ASSERT_OK(co_await meta.close({SUPER_USER, open1->stat.id, session2, true, {}, {}}));

      auto open3 = co_await meta.open({SUPER_USER, fname, session3, O_TRUNC | O_RDWR});
      CO_ASSERT_NE(open3->stat.id, open1->stat.id);
    }
  }());
}

TYPED_TEST(TestOpen, ConflictSet) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &store = cluster.meta().getStore();
    auto &meta = cluster.meta().getOperator();
    auto &storage = cluster.meta().getStorageClient();

    {
      auto path = "open-trunc-exists-file";
      auto result = co_await meta.create({SUPER_USER, path, {}, O_RDONLY, p644});
      CO_ASSERT_OK(result);
      auto session = MetaTestHelper::randomSession();
      InodeId inodeId = result->stat.id;
      CHECK_CONFLICT_SET(
          [&](auto &txn) -> CoTask<void> {
            auto req = OpenReq(SUPER_USER, Path(path), session, O_TRUNC | O_WRONLY);
            auto openResult = co_await store.open(req)->run(txn);
            CO_ASSERT_OK(openResult);
            CO_ASSERT_EQ(inodeId, openResult->stat.id);
            inodeId = openResult->stat.id;
          },
          (std::vector<String>{}),
          (std::vector<String>{
              MetaTestHelper::getInodeSessionKey(FileSession::create(inodeId, session)),
              // MetaTestHelper::getClientSessionKey(FileSession::create(inodeId, session))
          }),
          true);
    }

    {
      auto path = "open-trunc-file-by-inode";
      auto result = co_await meta.create({SUPER_USER, path, {}, O_RDONLY, p644});
      CO_ASSERT_OK(result);
      auto inodeId = result->stat.id;
      co_await truncate(meta, storage, result->stat, 2_GB);
      auto session = MetaTestHelper::randomSession();
      CHECK_CONFLICT_SET(
          [&](auto &txn) -> CoTask<void> {
            auto req = OpenReq(SUPER_USER, inodeId, session, O_TRUNC | O_WRONLY);
            auto openResult = co_await store.open(req)->run(txn);
            CO_ASSERT_OK(openResult);
            CO_ASSERT_EQ(openResult->stat.id, inodeId);
          },
          (std::vector<String>{}),
          (std::vector<String>{
              MetaTestHelper::getInodeSessionKey(FileSession::create(inodeId, session)),
              // MetaTestHelper::getClientSessionKey(FileSession::create(inodeId, session))
          }),
          false);
    }

    {
      auto path = "open-trunc-replace-exists-file";
      auto result = co_await meta.create({SUPER_USER, path, {}, O_RDONLY, p644});
      CO_ASSERT_OK(result);
      auto oldInodeId = result->stat.id;
      co_await truncate(meta, storage, result->stat, 2_GB);
      auto session = MetaTestHelper::randomSession();
      InodeId newInodeId;
      CHECK_CONFLICT_SET(
          [&](auto &txn) -> CoTask<void> {
            auto req = OpenReq(SUPER_USER, Path(path), session, O_TRUNC | O_WRONLY);
            auto openResult = co_await store.open(req)->run(txn);
            CO_ASSERT_OK(openResult);
            newInodeId = openResult->stat.id;
            CO_ASSERT_NE(oldInodeId, newInodeId);
          },
          (std::vector<String>{MetaTestHelper::getDirEntryKey(InodeId::root(), path),
                               MetaTestHelper::getInodeKey(oldInodeId)}),
          (std::vector<String>{
              MetaTestHelper::getDirEntryKey(InodeId::root(), path),
              MetaTestHelper::getInodeKey(newInodeId),
              MetaTestHelper::getInodeSessionKey(FileSession::create(newInodeId, session)),
              // MetaTestHelper::getClientSessionKey(FileSession::create(newInodeId, session))
          }),
          false);
    }
  }());
}

}  // namespace
}  // namespace hf3fs::meta::server
