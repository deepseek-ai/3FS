#pragma once

#include <algorithm>
#include <experimental/array>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/Synchronized.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/logging/xlog.h>
#include <functional>
#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <stack>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "client/mgmtd/MgmtdClientForServer.h"
#include "common/kv/IKVEngine.h"
#include "common/kv/ITransaction.h"
#include "common/kv/mem/MemKV.h"
#include "common/kv/mem/MemKVEngine.h"
#include "common/kv/mem/MemTransaction.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/Status.h"
#include "common/utils/StatusCode.h"
#include "common/utils/String.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Common.h"
#include "fbs/mgmtd/ChainRef.h"
#include "fbs/mgmtd/ChainSetting.h"
#include "fdb/FDB.h"
#include "fdb/FDBKVEngine.h"
#include "fdb/FDBTransaction.h"
#include "meta/base/Config.h"
#include "meta/components/ChainAllocator.h"
#include "meta/components/InodeIdAllocator.h"
#include "meta/components/SessionManager.h"
#include "meta/service/MetaOperator.h"
#include "meta/service/MockMeta.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "mgmtd/service/MgmtdOperator.h"
#include "mgmtd/service/MockMgmtd.h"
#include "tests/FakeMgmtdClient.h"
#include "tests/GtestHelpers.h"
#include "tests/fdb/KvTraits.h"
#include "tests/fdb/SetupFDB.h"

#define RANDOM_SLEEP_MS(ms)                                                 \
  do {                                                                      \
    auto dur = std::chrono::microseconds(folly::Random::rand32((ms)*1000)); \
    co_await folly::coro::sleep(dur);                                       \
  } while (0)

#define READ_ONLY_TRANSACTION(code_to_run)                    \
  do {                                                        \
    auto txn = this->kvEngine()->createReadonlyTransaction(); \
    code_to_run;                                              \
  } while (0)

#define READ_WRITE_TRANSACTION_ERROR(code_to_run, expected_code) \
  do {                                                           \
    auto txn = this->kvEngine()->createReadWriteTransaction();   \
    { code_to_run; }                                             \
    auto txnResult = co_await txn->commit();                     \
    CO_ASSERT_ERROR(txnResult, expected_code);                   \
  } while (0)

#define READ_WRITE_TRANSACTION_OK(code_to_run)                                     \
  do {                                                                             \
    auto txn = this->kvEngine()->createReadWriteTransaction();                     \
    { code_to_run; }                                                               \
    auto txnResult = co_await txn->commit();                                       \
    CO_ASSERT_FALSE(txnResult.hasError()) << fmt::format("{}", txnResult.error()); \
  } while (0)

#define READ_WRITE_TRANSACTION_NO_COMMIT(code_to_run)          \
  do {                                                         \
    auto txn = this->kvEngine()->createReadWriteTransaction(); \
    { code_to_run; }                                           \
  } while (0)

#define LOAD_INODE(name, inodeId)                   \
  Inode name(inodeId);                              \
  READ_ONLY_TRANSACTION({                           \
    auto result = co_await name.snapshotLoad(*txn); \
    CO_ASSERT_OK(result);                           \
    CO_ASSERT_TRUE(result->has_value());            \
  });

#define CO_ASSERT_DIRENTRY_EXISTS(parent, name)                            \
  READ_ONLY_TRANSACTION({                                                  \
    auto loadResult = co_await DirEntry::snapshotLoad(*txn, parent, name); \
    CO_ASSERT_OK(loadResult);                                              \
    CO_ASSERT_TRUE(loadResult->has_value());                               \
  })

#define CO_ASSERT_INODE_EXISTS(inodeId)                            \
  READ_ONLY_TRANSACTION({                                          \
    auto loadResult = co_await Inode::snapshotLoad(*txn, inodeId); \
    CO_ASSERT_OK(loadResult);                                      \
    CO_ASSERT_TRUE(loadResult->has_value());                       \
  })

#define CO_ASSERT_INODE_NOT_EXISTS(inodeId)                        \
  READ_ONLY_TRANSACTION({                                          \
    auto loadResult = co_await Inode::snapshotLoad(*txn, inodeId); \
    CO_ASSERT_OK(loadResult);                                      \
    CO_ASSERT_FALSE(loadResult->has_value());                      \
  })

#define GET_KV_CNTS(val, begin, end) \
  size_t val = 0;                    \
  READ_ONLY_TRANSACTION({ val = co_await MetaTestHelper::getKeyCountInRange(*txn, {begin, false}, {end, false}); })

#define GET_INODE_CNTS(val) GET_KV_CNTS(val, "INOD", "INOE")
#define GET_DIRENTRY_CNTS(val) GET_KV_CNTS(val, "DENT", "DENU")
#define GET_SESSION_CNTS(val) GET_KV_CNTS(val, "INOS", "INOT")

#define CO_ASSERT_KV_CNTS(val, begin, end) \
  do {                                     \
    GET_KV_CNTS(actual, begin, end);       \
    CO_ASSERT_EQ(actual, val);             \
  } while (0)
#define CO_ASSERT_INODE_CNTS(val) CO_ASSERT_KV_CNTS(val, "INOD", "INOE")
#define CO_ASSERT_DIRENTRY_CNTS(val) CO_ASSERT_KV_CNTS(val, "DENT", "DENU")
#define CO_ASSERT_SESSION_CNTS(val) CO_ASSERT_KV_CNTS(val, "INOS", "INOT")

#define CHECK_CONFLICT_SET(op, readSet, writeSet, exactly)                        \
  do {                                                                            \
    auto txn = this->kvEngine()->createReadWriteTransaction();                    \
    co_await op(*txn);                                                            \
    auto ok = MetaTestHelper::checkConflictSet(*txn, readSet, writeSet, exactly); \
    CO_ASSERT_TRUE(ok);                                                           \
  } while (0)

// check two operations not conflict
#define CO_ASSERT_NO_CONFLICT(op1, op2)                                                       \
  do {                                                                                        \
    auto txn1 = this->kvEngine()->createReadWriteTransaction();                               \
    auto txn2 = this->kvEngine()->createReadWriteTransaction();                               \
    co_await op1(*txn1);                                                                      \
    co_await op2(*txn2);                                                                      \
    auto *first = txn1.get(), *second = txn2.get();                                           \
    if (folly::Random::oneIn(2)) {                                                            \
      std::swap(first, second);                                                               \
    }                                                                                         \
    if (dynamic_cast<MemTransaction *>(first) && dynamic_cast<MemTransaction *>(second)) {    \
      auto conflict = MemTransaction::checkConflict(*dynamic_cast<MemTransaction *>(first),   \
                                                    *dynamic_cast<MemTransaction *>(second)); \
      CO_ASSERT_FALSE(conflict);                                                              \
      auto &memkv = dynamic_cast<MemTransaction *>(first)->kv();                              \
      memkv.backup();                                                                         \
      auto secondResult = co_await second->commit();                                          \
      auto firstResult = co_await first->commit();                                            \
      CO_ASSERT_OK(firstResult);                                                              \
      CO_ASSERT_OK(secondResult);                                                             \
      memkv.restore();                                                                        \
    }                                                                                         \
    auto firstResult = co_await first->commit();                                              \
    auto secondResult = co_await second->commit();                                            \
    CO_ASSERT_OK(firstResult);                                                                \
    CO_ASSERT_OK(secondResult);                                                               \
  } while (0)

// op1 commit first, op2 should fail with conflict
#define CO_ASSERT_CONFLICT(op1, op2)                                                                \
  do {                                                                                              \
    auto txn1 = this->kvEngine()->createReadWriteTransaction();                                     \
    auto txn2 = this->kvEngine()->createReadWriteTransaction();                                     \
    co_await op1(*txn1);                                                                            \
    co_await op2(*txn2);                                                                            \
    if (dynamic_cast<MemTransaction *>(txn1.get()) && dynamic_cast<MemTransaction *>(txn2.get())) { \
      auto conflict = MemTransaction::checkConflict(*dynamic_cast<MemTransaction *>(txn1.get()),    \
                                                    *dynamic_cast<MemTransaction *>(txn2.get()));   \
      CO_ASSERT_TRUE(conflict);                                                                     \
    }                                                                                               \
    auto firstResult = co_await txn1->commit();                                                     \
    auto secondResult = co_await txn2->commit();                                                    \
    CO_ASSERT_OK(firstResult);                                                                      \
    CO_ASSERT_ERROR(secondResult, TransactionCode::kConflict);                                      \
  } while (0)

#define FAULT_INJECTION_CHECK(result)                                     \
  do {                                                                    \
    if (result.hasError()) {                                              \
      bool ok = !TransactionHelper::isRetryable(result.error(), false) && \
                TransactionHelper::isTransactionError(result.error());    \
      CO_ASSERT_TRUE(ok) << result.error().describe();                    \
    }                                                                     \
  } while (0)

#define FAULT_INJECTION_CHECK_ERROR(result, expected)                   \
  do {                                                                  \
    CO_ASSERT_TRUE(result.hasError());                                  \
    if (result.error().code() != expected) {                            \
      bool ok = !TransactionHelper::isRetryable(result.error(), false); \
      CO_ASSERT_TRUE(ok) << result.error().describe();                  \
    }                                                                   \
  } while (0)

#define REQ(...) \
  { __VA_ARGS__ }

#define SUPER_USER \
  flat::UserInfo { Uid(0), Gid(0), String() }

namespace hf3fs::meta::server {
using namespace ::hf3fs::kv;

class MetaTestHelper {
 public:
  static String getInodeKey(const InodeId &id) {
    Inode inode(id);
    return inode.packKey();
  }

  static String getDirEntryKey(const InodeId &id, std::string_view name) {
    DirEntry entry(id, std::string(name));
    return entry.packKey();
  }

  static InodeId randomInodeId() {
    static folly::ConcurrentHashMap<uint64_t, Void> map;
    do {
      auto id = folly::Random::rand64(1 << 16, InodeId::normalMax().u64());
      auto [iter, insert] = map.insert(id, Void{});
      if (insert) {
        return InodeId(id);
      }
    } while (true);
  }

  static SessionInfo randomSession(std::optional<Uuid> client = {}) {
    return SessionInfo(ClientId(client.value_or(Uuid::random())), Uuid::random());
  }

  static auto randomEmptyLayout() {
    return Layout::newEmpty(ChainTableId(1),
                            ChainTableVersion(1),
                            1 << folly::Random::rand32(10, 32),
                            folly::Random::rand32(1, 256));
  }

  static auto randomLayout() {
    auto layout = Layout::newEmpty(ChainTableId(1),
                                   ChainTableVersion(1),
                                   1 << folly::Random::rand32(10, 32),
                                   folly::Random::rand32(1, 256));
    if (folly::Random::oneIn(2)) {
      layout.chains = Layout::ChainRange(folly::Random::rand32(1, 512),
                                         Layout::ChainRange::STD_SHUFFLE_MT19937,
                                         folly::Random::rand64());
    } else {
      std::vector<uint32_t> chains;
      for (size_t i = 0; i < layout.stripeSize; i++) {
        chains.push_back(folly::Random::rand32(1, std::numeric_limits<uint32_t>::max()));
      }
      layout.chains = Layout::ChainList{chains};
    }
    return layout;
  }

  static Inode randomInode();
  static Inode randomFile();
  static Inode randomDirectory();

  static DirEntry randomDirEntry();

  static bool checkConflictSet(IReadOnlyTransaction &txn,
                               const std::vector<String> &readConflict,
                               const std::vector<String> &writeConflict,
                               bool exactly) {
    auto memTxn = dynamic_cast<MemTransaction *>(&txn);
    if (memTxn) {
      auto ok = memTxn->checkConflictSet(readConflict, writeConflict, exactly);
      return ok;
    }
    return true;
  }

  static String getInodeSessionKey(const FileSession &session) {
    return FileSession::packKey(session.inodeId, session.sessionId);
  }

  // static String getClientSessionKey(const FileSession &session) {
  //   return FileSession::packKey(session.clientId.uuid, session.sessionId);
  // }

  [[nodiscard]] static CoTask<size_t> getKeyCountInRange(IReadOnlyTransaction &txn,
                                                         IReadOnlyTransaction::KeySelector begin,
                                                         const IReadOnlyTransaction::KeySelector end) {
    size_t total = 0;
    std::string prev;
    while (true) {
      auto range = co_await txn.snapshotGetRange(begin, end, 65535);
      EXPECT_TRUE(!range.hasError()) << range.error().describe();
      if (range.hasError()) break;
      total += range->kvs.size();
      if (range->hasMore) {
        prev = range->kvs.rbegin()->key;
        begin = {prev, false};
      } else {
        break;
      }
    }
    co_return total;
  }
};

template <typename KV>
class KvTestBase : public testing::SetupFDB {
 public:
  KvTestBase();
  ~KvTestBase() override = default;

  std::shared_ptr<IKVEngine> &kvEngine() { return engine_; }

 protected:
  void SetUp() override;
  void TearDown() override;

  std::shared_ptr<IKVEngine> createEngine();

  bool skip_;
  std::shared_ptr<IKVEngine> engine_;
};

class MockCluster {
 public:
  struct Config : ConfigBase<Config> {
    struct ChainTableConfig : ConfigBase<ChainTableConfig> {
      CONFIG_ITEM(table_id, 1u);
      CONFIG_ITEM(num_chains, 128u);
      CONFIG_ITEM(num_replica, 1u);
    };

    CONFIG_ITEM(num_meta, 3);
    CONFIG_OBJ(mock_meta, meta::server::Config, [](meta::server::Config &cfg) {
      cfg.set_iflags_chain_allocation(true);
      cfg.set_inodeId_check_unique(true);
      cfg.set_inodeId_abort_on_duplicate(true);
      cfg.set_check_file_hole(false);
      cfg.set_otrunc_replace_file(true);
      cfg.set_otrunc_replace_file_threshold(1_GB);
      cfg.session_manager().set_enable(false);
      cfg.session_manager().set_scan_interval(10_ms);
      cfg.gc().set_check_session(true);
      cfg.gc().set_scan_interval(10_ms);
      cfg.gc().set_retry_delay(100_ms);
      cfg.distributor().set_update_interval(50_ms);
      cfg.set_dynamic_stripe(true);
      cfg.set_enable_new_chunk_engine(folly::Random::oneIn(2));
      cfg.set_idempotent_rename(true);
      cfg.event_trace_log().set_enabled(true);
      cfg.event_trace_log().set_dump_interval(5_s);
      cfg.event_trace_log().set_max_num_writers(8);
    });
    CONFIG_OBJ(mock_mgmtd, mgmtd::MockMgmtd::Config);
    CONFIG_OBJ(mgmtd_client, client::MgmtdClientForServer::Config, [](client::MgmtdClientForServer::Config &cfg) {
      cfg.set_enable_auto_heartbeat(false);  // currently heartbeat is not needed in meta test
      cfg.set_mgmtd_server_addresses({net::NamedAddress::from("TCP://127.0.0.1:8000").value()});  // just a fake TCP address.
    });

    CONFIG_OBJ_ARRAY(chain_tables, ChainTableConfig, 64, [](std::array<ChainTableConfig, 64> &cfg) {
      cfg[0].set_table_id(1);
      cfg[0].set_num_chains(100 * 16);
      cfg[0].set_num_replica(1);
      cfg[1].set_table_id(2);
      cfg[1].set_num_chains(64 * 16);
      cfg[1].set_num_replica(2);
      cfg[2].set_table_id(3);
      cfg[2].set_num_chains(180 * 16);
      cfg[2].set_num_replica(2);
      return 3;
    });
  };

  static MockCluster create(std::shared_ptr<IKVEngine> engine, const Config &config) {
    MockCluster cluster(std::move(engine), config);
    cluster.SetUp();
    return cluster;
  }

  ~MockCluster();

  const Config &config() { return config_; }

  std::shared_ptr<hf3fs::tests::FakeMgmtdClient> mgmtdClient() { return mgmtdClient_; }

  MockMeta &meta() { return *meta_; }

  std::shared_ptr<IKVEngine> kvEngine() { return engine_; }

 private:
  MockCluster(std::shared_ptr<IKVEngine> engine, const Config &config)
      : config_(config),
        engine_(engine) {}
  MockCluster(MockCluster &&) = default;

  void SetUp();

  const Config &config_;
  std::shared_ptr<IKVEngine> engine_;

  std::unique_ptr<CPUExecutorGroup> exec_;
  std::shared_ptr<hf3fs::tests::FakeMgmtdClient> mgmtdClient_;
  std::unique_ptr<MockMeta> meta_;
};

template <typename KV>
class MetaTestBase : public KvTestBase<KV> {
 public:
  MockCluster createMockCluster() {
    static MockCluster::Config config = {};
    return MockCluster::create(this->kvEngine(), config);
  }

  MockCluster createMockCluster(const MockCluster::Config &config) {
    return MockCluster::create(this->kvEngine(), config);
  }
};

CoTask<void> randomWrite(MetaOperator &meta,
                         storage::client::StorageClient &storage,
                         const Inode &inode,
                         uint64_t offset,
                         uint64_t length);
CoTask<void> truncate(MetaOperator &meta, storage::client::StorageClient &storage, const Inode &inode, uint64_t length);

CoTryTask<size_t> queryTotalChunks(storage::client::StorageClient &storage, client::ICommonMgmtdClient &mgmtd);

CoTryTask<void> printTree(MetaOperator &meta);

inline const Uid rootUid{0};
inline const Gid rootGid{0};

inline const Permission p777{0777};
inline const Permission p755{0755};
inline const Permission p700{0700};
inline const Permission p644{0644};

inline const Acl rootp777{rootUid, rootGid, p777};
inline const Acl rootp755{rootUid, rootGid, p755};
inline const Acl rootp700{rootUid, rootGid, p700};
inline const Acl rootp644{rootUid, rootGid, p644};

}  // namespace hf3fs::meta::server
