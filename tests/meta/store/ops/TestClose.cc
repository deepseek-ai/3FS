#include <algorithm>
#include <atomic>
#include <chrono>
#include <fcntl.h>
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
#include <tuple>
#include <vector>

#include "client/storage/StorageClient.h"
#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "common/utils/UtcTime.h"
#include "fbs/storage/Common.h"
#include "gtest/gtest.h"
#include "meta/components/SessionManager.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {
namespace {

template <typename KV>
class TestClose : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestClose, KVTypes);

TYPED_TEST(TestClose, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    // close directory
    CO_ASSERT_ERROR(
        co_await meta.close(
            {SUPER_USER, InodeId::root(), MetaTestHelper::randomSession(), true, std::nullopt, std::nullopt}),
        MetaCode::kNotFile);

    auto create = co_await meta.create({SUPER_USER, PathAt("test-file"), {}, O_EXCL, p644});
    CO_ASSERT_OK(create);
    auto inode = create->stat;
    CO_ASSERT_EQ(inode.asFile().length, 0);

    // file has no session
    READ_WRITE_TRANSACTION_NO_COMMIT({
      auto result = co_await FileSession::checkExists(*txn, inode.id);
      CO_ASSERT_OK(result);
      CO_ASSERT_FALSE(*result);
    });

    // open file with read
    CO_ASSERT_OK(co_await meta.open({SUPER_USER, PathAt("test-file"), {}, O_RDONLY}));
    // file has no session
    READ_WRITE_TRANSACTION_NO_COMMIT({
      auto result = co_await FileSession::checkExists(*txn, inode.id);
      CO_ASSERT_OK(result);
      CO_ASSERT_FALSE(*result);
    });

    // open file with write
    auto session = MetaTestHelper::randomSession();
    CO_ASSERT_OK(co_await meta.open({SUPER_USER, PathAt("test-file"), session, O_WRONLY}));

    // file has session & client has session
    READ_WRITE_TRANSACTION_NO_COMMIT({
      auto result = co_await FileSession::checkExists(*txn, inode.id);
      CO_ASSERT_OK(result);
      CO_ASSERT_TRUE(*result);
      auto inodeSessions = co_await FileSession::list(*txn, inode.id, false);
      CO_ASSERT_TRUE(inodeSessions.hasValue() && !inodeSessions->empty());
      // auto clientSessions = co_await FileSession::list(*txn, session.client.uuid, false);
      // CO_ASSERT_TRUE(clientSessions.hasValue() && !clientSessions->empty());
    });

    // close file, should set session if update length is enabled
    CO_ASSERT_OK(co_await meta.close({SUPER_USER, inode.id, {}, false, UtcClock::now(), {}}));
    CO_ASSERT_ERROR(co_await meta.close({SUPER_USER, inode.id, {}, true, UtcClock::now(), {}}),
                    StatusCode::kInvalidArg);

    CO_ASSERT_OK(co_await meta.close({SUPER_USER, inode.id, session, true, {}, {}}));

    // file has no session, client has no session
    READ_WRITE_TRANSACTION_NO_COMMIT({
      auto result = co_await FileSession::checkExists(*txn, inode.id);
      CO_ASSERT_OK(result);
      CO_ASSERT_FALSE(*result);
      auto inodeSessions = co_await FileSession::list(*txn, inode.id, false);
      CO_ASSERT_TRUE(inodeSessions.hasValue() && inodeSessions->empty());
      // todo: maybe should check session count
      // or: just dump all sessions
      // auto clientSessions = co_await FileSession::list(*txn, session.client.uuid, false);
      // CO_ASSERT_TRUE(clientSessions.hasValue() && clientSessions->empty());
    });
  }());
}

// TYPED_TEST(TestClose, CheckHole) {
//   folly::coro::blockingWait([&]() -> CoTask<void> {
//     MockCluster::Config cfg;
//     cfg.mock_meta().set_check_file_hole(true);
//     auto cluster = this->createMockCluster(cfg);
//     auto &meta = cluster.meta().getOperator();
//     auto &storage = cluster.meta().getStorageClient();

//     auto create = co_await meta.create({SUPER_USER, PathAt("test-file"), {}, O_EXCL, p644});
//     CO_ASSERT_OK(create);
//     Inode inode(create->stat);
//     CO_ASSERT_EQ(inode.asFile().length, 0);

//     auto session1 = MetaTestHelper::randomSession();
//     auto session2 = MetaTestHelper::randomSession();
//     CO_ASSERT_OK(co_await meta.open({SUPER_USER, PathAt("test-file"), session1, O_WRONLY}));
//     CO_ASSERT_OK(co_await meta.open({SUPER_USER, PathAt("test-file"), session2, O_WRONLY}));

//     // do some write to generate hole
//     co_await randomWrite(meta, storage, inode, 1 << 20, 1 << 20);
//     // close session1, return ok
//     CO_ASSERT_OK(co_await meta.close({SUPER_USER, inode.id, session1, true, {}, {}}));

//     // close session2, return has hole
//     CO_ASSERT_ERROR(co_await meta.close({SUPER_USER, inode.id, session2, true, {}, {}}), MetaCode::kFileHasHole);

//     // don't allow open O_RDONLY
//     CO_ASSERT_ERROR(co_await meta.open({SUPER_USER, PathAt("test-file"), {}, O_RDONLY}), MetaCode::kFileHasHole);

//     // open read write
//     auto session3 = MetaTestHelper::randomSession();
//     CO_ASSERT_OK(co_await meta.open({SUPER_USER, PathAt("test-file"), session3, O_WRONLY}));

//     // write fix hole
//     co_await randomWrite(meta, storage, inode, 0, 3 << 20);
//     // close should ok
//     CO_ASSERT_OK(co_await meta.close({SUPER_USER, inode.id, session3, true, {}, {}}));

//     // open and check length
//     auto oresult = co_await meta.open({SUPER_USER, PathAt("test-file"), {}, O_RDONLY});
//     CO_ASSERT_OK(oresult);
//     CO_ASSERT_EQ(oresult->stat.asFile().length, 3 << 20);
//   }());
// }

TYPED_TEST(TestClose, Concurrent) {
  folly::CPUThreadPoolExecutor exec(8);
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config cfg;
    cfg.mock_meta().set_check_file_hole(true);
    auto cluster = this->createMockCluster(cfg);
    auto &meta = cluster.meta().getOperator();
    auto &storage = cluster.meta().getStorageClient();

    auto worker = [&](Path path,
                      size_t offset,
                      size_t length,
                      std::atomic_int *failed,
                      folly::futures::Barrier *beginBarr,
                      folly::futures::Barrier *endBarr) -> CoTask<void> {
      // open, should succ
      if (!beginBarr && folly::Random::oneIn(4)) {
        co_await folly::coro::sleep(std::chrono::milliseconds(folly::Random::rand32(300)));
      }
      auto session = MetaTestHelper::randomSession();
      auto oresult = co_await meta.create({SUPER_USER, path, session, O_RDWR, p644});
      CO_ASSERT_OK(oresult);
      Inode inode = oresult->stat;
      if (beginBarr) co_await beginBarr->wait();

      co_await randomWrite(meta, storage, inode, offset, length);

      if (endBarr) co_await endBarr->wait();
      auto cresult = co_await meta.close({SUPER_USER, inode.id, session, true, {}, UtcClock::now()});
      if (cresult.hasError()) {
        CO_ASSERT_ERROR(cresult, MetaCode::kFileHasHole);
        if (failed) (*failed)++;
      }
      co_return;
    };

    std::vector<std::tuple<bool, bool, bool>> tests;
    for (auto hole : {false, true}) {
      for (auto beginBarr : {false, true})
        for (auto endBarr : {false, true}) tests.push_back({beginBarr, endBarr, hole});
    }

    for (auto [hole, beginBarr, endBarr] : tests) {
      XLOGF(ERR, "test: beginBarr {}, endBarr {}, hole {}", beginBarr, endBarr, hole);
      Path path = fmt::format("file-{}", folly::Random::rand32());
      folly::futures::Barrier barr1(16), barr2(16);
      std::vector<folly::SemiFuture<folly::Unit>> futures;
      std::atomic_int failed(0);
      size_t offset = hole ? folly::Random::rand32(1 << 20, 8 << 20) : 0;
      for (size_t i = 0; i < 16; i++) {
        size_t length = folly::Random::rand32(128 << 10, 2 << 20);
        auto task = folly::coro::co_invoke(worker,
                                           path,
                                           offset,
                                           length,
                                           &failed,
                                           beginBarr ? &barr1 : nullptr,
                                           endBarr ? &barr2 : nullptr)
                        .scheduleOn(&exec)
                        .start();
        futures.push_back(std::move(task));
        offset += length;
      }
      for (auto &f : futures) {
        f.wait();
      }

      // todo: support check hole in future
      // if (!hole) {
      //   if (beginBarr) {
      //     CO_ASSERT_EQ(failed, 0);
      //   }
      //   CO_ASSERT_OK(co_await meta.open({SUPER_USER, path, {}, O_RDONLY}));
      // } else {
      //   XLOGF(ERR, "{} close found hole", failed.load());
      //   CO_ASSERT_NE(failed, 0);
      //   CO_ASSERT_ERROR(co_await meta.open({SUPER_USER, path, {}, O_RDONLY}), MetaCode::kFileHasHole);
      // }
    }

    co_return;
  }());
}

}  // namespace
}  // namespace hf3fs::meta::server
