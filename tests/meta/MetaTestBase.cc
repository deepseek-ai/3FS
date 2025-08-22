#include "MetaTestBase.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fmt/core.h>
#include <folly/Conv.h>
#include <folly/Math.h>
#include <folly/Random.h>
#include <folly/Unit.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unistd.h>

#include "client/storage/StorageClient.h"
#include "common/app/ClientId.h"
#include "common/kv/IKVEngine.h"
#include "common/kv/ITransaction.h"
#include "common/kv/mem/MemKV.h"
#include "common/kv/mem/MemKVEngine.h"
#include "common/kv/mem/MemTransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/FileOperation.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "fbs/mgmtd/ChainRef.h"
#include "fbs/mgmtd/ChainSetting.h"
#include "fdb/FDB.h"
#include "fdb/FDBKVEngine.h"
#include "fdb/FDBTransaction.h"
#include "foundationdb/fdb_c_types.h"
#include "meta/service/MetaOperator.h"
#include "meta/service/MockMeta.h"
#include "meta/store/MetaStore.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::meta::server {

namespace {
auto randomAcl() {
  return Acl(Uid(folly::Random::rand32()), Gid(folly::Random::rand32()), Permission(folly::Random::rand32()));
}
}  // namespace

Inode MetaTestHelper::randomInode() {
  switch (folly::Random::rand32() % 3) {
    case 0:
      return randomFile();
    case 1:
      return randomDirectory();
    default: {
      auto acl = randomAcl();
      auto name = std::to_string(folly::Random::rand32());
      return Inode::newSymlink(randomInodeId(), name, acl.uid, acl.gid, UtcClock::now().castGranularity(1_s));
    }
  }
}

Inode MetaTestHelper::randomFile() {
  return Inode::newFile(randomInodeId(), randomAcl(), randomLayout(), UtcClock::now().castGranularity(1_s));
}

Inode MetaTestHelper::randomDirectory() {
  return Inode::newDirectory(randomInodeId(),
                             randomInodeId(),
                             folly::to<std::string>(folly::Random::rand32()),
                             randomAcl(),
                             randomEmptyLayout(),
                             UtcClock::now().castGranularity(1_s));
}

DirEntry MetaTestHelper::randomDirEntry() {
  InodeId parent = MetaTestHelper::randomInodeId();
  String name = fmt::format("entry-{}", parent, folly::Random::rand64());
  InodeId id = MetaTestHelper::randomInodeId();
  auto type = InodeType(rand() % magic_enum::enum_count<InodeType>());
  DirEntry entry;
  switch (type) {
    case InodeType::File:
      entry = DirEntry::newFile(parent, name, id);
      break;
    case InodeType::Directory:
      entry = DirEntry::newDirectory(
          parent,
          name,
          id,
          Acl(Uid(folly::Random::rand32()), Gid(folly::Random::rand32()), Permission(folly::Random::rand32())));
      break;
    case InodeType::Symlink:
      entry = DirEntry::newSymlink(parent, name, id);
      break;
  }
  return entry;
}

template <typename KV>
KvTestBase<KV>::KvTestBase()
    : skip_(true),
      engine_(nullptr) {
  if constexpr (std::is_same<KV, mem::MemKV>::value) {
    skip_ = false;
  } else if constexpr (std::is_same<KV, fdb::DB>::value) {
    auto cluster = std::getenv("FDB_UNITTEST_CLUSTER");
    if (cluster) {
      skip_ = false;
    }
  } else {
    XLOGF(FATAL, "Invalid KV type {}", std::type_identity<KV>::type);
  }
}

template <typename KV>
std::shared_ptr<IKVEngine> KvTestBase<KV>::createEngine() {
  if (skip_) {
    throw std::runtime_error("Shouldn't reach here!!!");
  }

  if constexpr (std::is_same<KV, mem::MemKV>::value) {
    return std::shared_ptr<IKVEngine>(new MemKVEngine());
  } else if constexpr (std::is_same<KV, fdb::DB>::value) {
    auto cluster = std::getenv("FDB_UNITTEST_CLUSTER");
    fdb::DB db(cluster, false);
    if (db.error()) {
      throw std::runtime_error(fmt::format("Failed to create fdb, error {}", db.error()));
    }
    return std::shared_ptr<IKVEngine>(new FDBKVEngine(std::move(db)));
  } else {
    XLOGF(FATAL, "Invalid KV type {}", std::type_identity<KV>::type);
  }
}

template <typename KV>
void KvTestBase<KV>::SetUp() {
  if (skip_) {
    GTEST_SKIP_("FDB_UNITTEST_CLUSTER not set");
  }

  // create KV engine
  engine_ = createEngine();
  if (dynamic_cast<FDBKVEngine *>(engine_.get())) {
    // this is FDB test, remove all kvs from FDB
    folly::coro::blockingWait([&]() -> CoTask<void> {
      auto txn = engine_->createReadWriteTransaction();
      auto clearResult = co_await dynamic_cast<FDBTransaction *>(txn.get())->clearRange("", "\xff");
      CO_ASSERT_OK(clearResult);
      auto result = co_await txn->commit();
      CO_ASSERT_OK(result);
      XLOGF(INFO, "FDB cleared, result {}", result.hasError() ? result.error().describe() : "ok");
    }());
  }
}

template <typename KV>
void KvTestBase<KV>::TearDown() {
  // do nothing
}

template class KvTestBase<mem::MemKV>;
template class KvTestBase<fdb::DB>;

static const String clusterId = "mock-cluster";

void MockCluster::SetUp() {
  ReqBase::currentClientId() = ClientId(Uuid::random(), "meta_test");
  exec_ = std::make_unique<CPUExecutorGroup>(4, clusterId);

  std::vector<std::tuple<flat::ChainTableId, size_t, size_t>> tables;
  for (unsigned i = 0; i < config_.chain_tables_length(); i++) {
    auto table = flat::ChainTableId(config_.chain_tables(i).table_id());
    auto numChains = config_.chain_tables(i).num_chains();
    auto numReplica = config_.chain_tables(i).num_replica();
    tables.emplace_back(table, numChains, numReplica);
  }
  mgmtdClient_ = hf3fs::tests::FakeMgmtdClient::create(tables, config_.num_meta(), 10);

  auto meta = folly::coro::blockingWait(MockMeta::create(config_.mock_meta(), engine_, mgmtdClient_));
  ASSERT_OK(meta);
  meta_ = std::move(*meta);
  meta_->start(*exec_);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

MockCluster::~MockCluster() {
  ReqBase::currentClientId() = std::nullopt;
  if (meta_) {
    meta_->stop();
  }
}

CoTask<void> randomWrite(MetaOperator &meta,
                         storage::client::StorageClient &storage,
                         const Inode &inode,
                         uint64_t offset,
                         uint64_t length) {
  auto stripe = std::min((uint32_t)folly::divCeil(offset + length, (uint64_t)inode.asFile().layout.chunkSize),
                         inode.asFile().layout.stripeSize);
  if (inode.asFile().dynStripe && inode.asFile().dynStripe < stripe) {
    auto result = co_await meta.setAttr(SetAttrReq::extendStripe(flat::UserInfo{}, inode.id, stripe));
    CO_ASSERT_OK(result);
  }

  uint64_t chunkSize = inode.asFile().layout.chunkSize;
  std::vector<uint8_t> writeData(chunkSize, 0x00);
  std::vector<folly::SemiFuture<folly::Unit>> tasks;
  while (length) {
    auto offsetInChunk = offset % chunkSize;
    auto lengthInChunk = std::min(length, chunkSize - offsetInChunk);
    auto chunkId = inode.asFile().getChunkId(inode.id, offset);
    auto routingInfo = storage.getMgmtdClient().getRoutingInfo()->raw();
    XLOGF_IF(FATAL, !routingInfo, "No routingInfo");
    auto chainId = inode.asFile().getChainId(inode, offset, *routingInfo);
    XLOGF_IF(FATAL, !chainId, "resolve chainId failed: {}", chainId);

    auto task = [=, &storage, &writeData]() -> CoTask<void> {
      auto writeIO = storage.createWriteIO(storage::ChainId(*chainId),
                                           storage::ChunkId(chunkId->pack()),
                                           offsetInChunk,
                                           lengthInChunk,
                                           chunkSize,
                                           writeData.data(),
                                           nullptr);
      XLOGF(DBG, "write {} offset {}, offsetInChunk {} length {}", chunkId, offset, offsetInChunk, lengthInChunk);
      auto result = co_await storage.write(writeIO, flat::UserInfo());
      CO_ASSERT_FALSE(result.hasError()) << result.error().describe();
      CO_ASSERT_FALSE(writeIO.result.lengthInfo.hasError()) << writeIO.result.lengthInfo.error().describe();
      CO_ASSERT_EQ(*writeIO.result.lengthInfo, lengthInChunk);
    };

    tasks.push_back(folly::coro::co_invoke(task).scheduleOn(co_await folly::coro::co_current_executor).start());

    offset += lengthInChunk;
    length -= lengthInChunk;
  }

  co_await folly::collectAll(tasks.begin(), tasks.end());
}

CoTask<void> truncate(MetaOperator &meta,
                      storage::client::StorageClient &storage,
                      const Inode &inode,
                      uint64_t length) {
  auto stripe = std::min((uint32_t)folly::divCeil(length, (uint64_t)inode.asFile().layout.chunkSize),
                         inode.asFile().layout.stripeSize);
  if (inode.asFile().dynStripe && inode.asFile().dynStripe < stripe) {
    auto result = co_await meta.setAttr(SetAttrReq::extendStripe(flat::UserInfo{}, inode.id, stripe));
    CO_ASSERT_OK(result);
  }

  CO_ASSERT_TRUE(inode.isFile()) << fmt::format("{}", inode);
  flat::UserInfo user{};
  auto routing = storage.getMgmtdClient().getRoutingInfo();
  auto fop = FileOperation(storage, *routing->raw(), user, inode);
  bool more = true;
  uint64_t removed = 0;
  while (more) {
    auto remove = co_await fop.removeChunks(length, 32, false, {});
    CO_ASSERT_OK(remove);
    removed += remove->first;
    more = remove->second;
    XLOGF(INFO, "truncate {} to {}, removed {}, more {}", inode.id, length, removed, more);
    CO_ASSERT_FALSE((!remove->first && more));
  }

  CO_ASSERT_OK(co_await fop.truncateChunk(length));
  CO_ASSERT_OK(co_await meta.sync({SUPER_USER, inode.id, true, std::nullopt, std::nullopt, true, std::nullopt}));
}

CoTryTask<size_t> queryTotalChunks(storage::client::StorageClient &storage, client::ICommonMgmtdClient &mgmtd) {
  ChunkId begin(InodeId(0), 0, 0), end(InodeId(-1), 0, 0);
  auto routing = mgmtd.getRoutingInfo();
  EXPECT_TRUE(routing);
  size_t total = 0;
  for (const auto &chain : routing->raw()->chains) {
    auto query =
        storage.createQueryOp(chain.first, storage::ChunkId(begin.pack()), storage::ChunkId(end.pack()), UINT32_MAX);
    CO_RETURN_ON_ERROR(co_await storage.queryLastChunk(std::span(&query, 1), {}));
    total += query.result.totalNumChunks;
  }
  co_return total;
}

CoTryTask<void> printTree(MetaOperator &meta) {
  auto queue = std::queue<std::pair<InodeId, Path>>();
  queue.push({InodeId::root(), "/"});
  auto map = std::map<Path, std::string>();
  while (!queue.empty()) {
    auto [inodeId, path] = queue.front();
    queue.pop();
    auto prev = std::string();
    while (true) {
      auto res = co_await meta.list({SUPER_USER, PathAt(inodeId), prev, 256, true});
      CO_RETURN_ON_ERROR(res);
      for (auto idx = 0ul; idx < res->entries.size(); idx++) {
        auto &entry = res->entries[idx];
        auto &inode = res->inodes[idx];
        prev = entry.name;
        auto entryPath = path / entry.name;
        switch (entry.type) {
          case InodeType::File:
            map[entryPath] = "File";
            break;
          case InodeType::Directory:
            map[entryPath] = "";
            queue.push({entry.id, entryPath});
            break;
          case InodeType::Symlink: {
            map[entryPath] = inode.asSymlink().target.string();
            break;
          }
        }
      }
      if (!res->more) {
        break;
      }
    }
  }

  for (auto &iter : map) {
    auto [path, msg] = iter;
    if (msg.empty()) {
      fmt::println("{}", path);
    } else {
      fmt::println("- {} -> {}", path, msg);
    }
  }

  co_return Void{};
}

}  // namespace hf3fs::meta::server
