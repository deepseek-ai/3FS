#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest-param-test.h>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/app/ClientId.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "gtest/gtest.h"
#include "meta/components/GcManager.h"
#include "meta/components/SessionManager.h"
#include "meta/store/FileSession.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

template <typename KV>
class TestSessionManager : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestSessionManager, KVTypes);

TYPED_TEST(TestSessionManager, basic) {
  // for (size_t i = 0; i < 10; i++) {
  //   auto client = Uuid::random();
  //   auto session = Uuid::random();
  //   auto key = FileSession::packKey(client, session);
  //   auto unpacked = FileSession::unpackByClientKey(key);
  //   ASSERT_EQ(unpacked->first, client);
  //   ASSERT_EQ(unpacked->second, session);
  // }

  for (size_t i = 0; i < 10; i++) {
    auto inode = InodeId(folly::Random::rand64());
    auto session = Uuid::random();
    auto key = FileSession::packKey(inode, session);
    auto unpacked = FileSession::unpackByInodeKey(key);
    ASSERT_EQ(unpacked->first, inode);
    ASSERT_EQ(unpacked->second, session);
  }
}

TYPED_TEST(TestSessionManager, byInode) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto mgmtd = std::dynamic_pointer_cast<tests::FakeMgmtdClient>(cluster.mgmtdClient());

    for (size_t i = 0; i < 10; i++) {
      auto inode = InodeId(folly::Random::rand64());
      size_t numSessions = folly::Random::rand32(5, 32);
      for (size_t j = 0; j < numSessions; j++) {
        READ_WRITE_TRANSACTION_OK({
          auto session = FileSession::create(inode, ClientId::random(), Uuid::random());
          CO_ASSERT_OK(co_await session.store(*txn));
        });
      }
      READ_ONLY_TRANSACTION({
        auto sessions = co_await FileSession::list(*txn, inode, false);
        CO_ASSERT_OK(sessions);
        CO_ASSERT_EQ(sessions->size(), numSessions);
        auto hasSession = co_await FileSession::snapshotCheckExists(*txn, inode);
        CO_ASSERT_OK(hasSession);
        CO_ASSERT_TRUE(*hasSession);
      });

      READ_WRITE_TRANSACTION_OK({ CO_ASSERT_OK(co_await FileSession::removeAll(*txn, inode)); });
      READ_WRITE_TRANSACTION_OK({
        auto sessions = co_await FileSession::list(*txn, inode, false);
        CO_ASSERT_OK(sessions);
        CO_ASSERT_EQ(sessions->size(), 0);
        auto hasSession = co_await FileSession::snapshotCheckExists(*txn, inode);
        CO_ASSERT_OK(hasSession);
        CO_ASSERT_FALSE(*hasSession);
      });
    }
  }());
}

TYPED_TEST(TestSessionManager, scan) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto mgmtd = std::dynamic_pointer_cast<tests::FakeMgmtdClient>(cluster.mgmtdClient());

    for (size_t i = 0; i < 100; i++) {
      READ_WRITE_TRANSACTION_OK({
        for (size_t i = 0; i < 500; i++) {
          auto inode = InodeId(folly::Random::rand64(1 << 10, std::numeric_limits<uint64_t>::max()));
          auto session = FileSession::create(inode, ClientId::random(), Uuid::random());
          CO_ASSERT_OK(co_await session.store(*txn));
          auto prune = FileSession::createPrune(session.clientId, session.sessionId);
          CO_ASSERT_OK(co_await prune.store(*txn));
        }
      });
    }

    std::map<Uuid, FileSession> sessions;
    for (size_t i = 0; i < FileSession::kShard; i++) {
      std::vector<FileSession> vec;
      bool more = true;
      while (more) {
        READ_ONLY_TRANSACTION({
          auto sessions = co_await FileSession::scan(*txn, i, !vec.empty() ? std::optional(vec.back()) : std::nullopt);
          CO_ASSERT_OK(sessions);
          more = !sessions->empty();
          vec.insert(vec.end(), sessions->begin(), sessions->end());
        });
      }
      for (auto &s : vec) {
        sessions.emplace(s.sessionId, s);
      }
    }
    CO_ASSERT_EQ(sessions.size(), 100 * 500);

    READ_ONLY_TRANSACTION({
      auto prune = co_await FileSession::listPrune(*txn, sessions.size() * 2);
      CO_ASSERT_OK(prune);
      CO_ASSERT_EQ(prune->size(), sessions.size());
      for (auto s : *prune) {
        CO_ASSERT_TRUE(sessions.find(s.sessionId) != sessions.end());
        sessions.erase(s.sessionId);
      }
    });
    CO_ASSERT_TRUE(sessions.empty());
  }());
}

// TYPED_TEST(TestSessionManager, byClient) {
//   folly::coro::blockingWait([&]() -> CoTask<void> {
//     auto cluster = this->createMockCluster();
//     auto &sessionManager = cluster.meta().getSessionManager();
//     auto mgmtd = std::dynamic_pointer_cast<tests::FakeMgmtdClient>(cluster.mgmtdClient());
//     for (auto clientId : mgmtd->getActiveClients()) {
//       size_t numSessions = folly::Random::rand32(5, 32);
//       for (size_t j = 0; j < numSessions; j++) {
//         READ_WRITE_TRANSACTION_OK({
//           auto session = FileSession::create(InodeId(folly::Random::rand32()), ClientId(clientId), Uuid::random());
//           CO_ASSERT_OK(co_await session.store(*txn));
//         });
//       }
//
//       READ_ONLY_TRANSACTION({
//         auto sessions = co_await FileSession::list(*txn, clientId, true);
//         CO_ASSERT_OK(sessions);
//         CO_ASSERT_EQ(sessions->size(), numSessions);
//       });
//     }
//
//     std::vector<std::pair<Uuid, size_t>> deadClients;
//     for (size_t i = 0; i < 10; i++) {
//       auto clientId = Uuid::random();
//       size_t numSessions = folly::Random::rand32(1, 32);
//       deadClients.emplace_back(clientId, numSessions);
//       for (size_t j = 0; j < numSessions; j++) {
//         READ_WRITE_TRANSACTION_OK({
//           auto session = FileSession::create(InodeId(folly::Random::rand32()), ClientId(clientId), Uuid::random());
//           CO_ASSERT_OK(co_await session.store(*txn));
//         });
//       }
//       READ_ONLY_TRANSACTION({
//         auto sessions = co_await FileSession::list(*txn, clientId, false);
//         CO_ASSERT_OK(sessions);
//         CO_ASSERT_EQ(sessions->size(), numSessions);
//       });
//     }
//     READ_ONLY_TRANSACTION({
//       auto all = co_await sessionManager.listClients();
//       CO_ASSERT_OK(all);
//       CO_ASSERT_EQ(all->size(), deadClients.size() + mgmtd->getActiveClients().size());
//       for (auto client : *all) {
//         auto active = mgmtd->getActiveClients().contains(client.clientId.uuid);
//         CO_ASSERT_EQ(active, client.active);
//         CO_ASSERT_NE(client.sessions, 0);
//       }
//     });
//   }());
// }

TYPED_TEST(TestSessionManager, prune) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().session_manager().set_enable(true);

    auto cluster = this->createMockCluster(config);
    auto mgmtd = std::dynamic_pointer_cast<tests::FakeMgmtdClient>(cluster.mgmtdClient());

    auto now = UtcClock::now().toMicroseconds();
    auto minutes = 60 * 1000000;

    std::vector<Uuid> deadClients;
    for (size_t i = 0; i < 100; i++) {
      deadClients.push_back(Uuid::random());
    }
    std::vector<FileSession> active, steal, future;
    for (size_t i = 0; i < folly::Random::rand32(1000); i++) {
      auto client = mgmtd->getOneActiveClient();
      READ_WRITE_TRANSACTION_OK({
        auto session = FileSession::create(InodeId(folly::Random::rand32()),
                                           client,
                                           Uuid::random(),
                                           UtcTime::fromMicroseconds(now - minutes));
        active.push_back(session);
        CO_ASSERT_OK(co_await session.store(*txn));

        auto pruneSessionId = Uuid::random();
        CO_ASSERT_OK(
            co_await FileSession::create(InodeId(folly::Random::rand32()), client, pruneSessionId).store(*txn));
        CO_ASSERT_OK(co_await FileSession::createPrune(client, pruneSessionId).store(*txn));
        CO_ASSERT_OK(co_await FileSession::createPrune(ClientId(Uuid::random()), pruneSessionId).store(*txn));
      });
    }
    for (size_t i = 0; i < folly::Random::rand32(1000); i++) {
      READ_WRITE_TRANSACTION_OK({
        auto session = FileSession::create(InodeId(folly::Random::rand32()),
                                           ClientId(deadClients.at(folly::Random::rand32(deadClients.size()))),
                                           Uuid::random(),
                                           UtcTime::fromMicroseconds(now - minutes));
        steal.push_back(session);
        CO_ASSERT_OK(co_await session.store(*txn));
      });
    }
    for (size_t i = 0; i < folly::Random::rand32(1000); i++) {
      READ_WRITE_TRANSACTION_OK({
        auto session = FileSession::create(InodeId(folly::Random::rand32()),
                                           ClientId(deadClients.at(folly::Random::rand32(deadClients.size()))),
                                           Uuid::random(),
                                           UtcTime::fromMicroseconds(now + minutes));
        future.push_back(session);
        CO_ASSERT_OK(co_await session.store(*txn));
      });
    }

    co_await folly::coro::sleep(std::chrono::seconds(1));
    fmt::print("{} {}\n", active.size(), future.size());
    CO_ASSERT_SESSION_CNTS(active.size() + future.size());

    mgmtd->clearActiveClient();
    co_await folly::coro::sleep(std::chrono::seconds(1));
    CO_ASSERT_SESSION_CNTS(future.size());
  }());
}

TYPED_TEST(TestSessionManager, syncOnPrune) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().set_sync_on_prune_session(true);
    config.mock_meta().session_manager().set_enable(true);
    config.mock_meta().session_manager().set_sync_on_prune_session(true);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();
    auto &storage = cluster.meta().getStorageClient();
    auto mgmtd = std::dynamic_pointer_cast<tests::FakeMgmtdClient>(cluster.mgmtdClient());

    auto result = co_await meta.create({SUPER_USER, PathAt("file"), std::nullopt, O_RDONLY, p644});
    CO_ASSERT_OK(result);
    auto &inode = result->stat;
    auto length = folly::Random::rand64(2 << 20, 4 << 20);
    co_await randomWrite(meta, storage, inode, 0, length);

    // create a dead session
    READ_WRITE_TRANSACTION_OK({
      auto session = FileSession::create(
          inode.id,
          ClientId(Uuid::random()),
          Uuid::random(),
          UtcTime::fromMicroseconds(UtcClock::now().toMicroseconds() - 60 * 60 * 1000000L /* 1h */));
      CO_ASSERT_OK(co_await session.store(*txn));
    });

    // wait sometime, inode should be synced
    CO_ASSERT_SESSION_CNTS(1);
    co_await folly::coro::sleep(std::chrono::seconds(3));
    CO_ASSERT_SESSION_CNTS(0);

    auto stat = co_await meta.stat({SUPER_USER, PathAt("file"), AtFlags()});
    CO_ASSERT_OK(stat);
    CO_ASSERT_EQ(stat->stat.asFile().length, length);
  }());
}

}  // namespace hf3fs::meta::server