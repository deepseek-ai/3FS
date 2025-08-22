#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>

#include "common/utils/Size.h"
#include "fbs/storage/Common.h"
#include "storage/aio/AioReadWorker.h"
#include "storage/service/TargetMap.h"
#include "storage/store/ChunkStore.h"
#include "storage/store/StorageTarget.h"
#include "storage/update/UpdateWorker.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::storage {
namespace {

TEST(TestStorageTarget, ReadWrite) {
  folly::test::TemporaryDirectory tmpPath;
  CPUExecutorGroup executor(8, "");

  const auto chunkId = ChunkId{12345, 12345};
  constexpr TargetId targetId{0};
  constexpr auto chunkSize = 1_MB;
  std::string dataBytes(chunkSize, 'B');
  ServiceRequestContext requestCtx;

  StorageTargets::Config storageTargetConfig;
  storageTargetConfig.set_target_num_per_path(1);
  storageTargetConfig.set_target_paths({tmpPath.path()});
  storageTargetConfig.storage_target().file_store().set_preopen_chunk_size_list({512_KB, 1_MB});
  storageTargetConfig.set_allow_disk_without_uuid(true);

  {
    StorageTargets::CreateConfig createConfig;
    createConfig.set_chunk_size_list({1_MB});
    createConfig.set_physical_file_count(8);
    createConfig.set_allow_disk_without_uuid(true);
    createConfig.set_target_ids({targetId});

    AtomicallyTargetMap targetMap;
    StorageTargets targets(storageTargetConfig, targetMap);
    ASSERT_OK(targets.create(createConfig));
  }

  for (auto loop = 0; loop < 3; ++loop) {
    AtomicallyTargetMap targetMap;
    StorageTargets storageTargets(storageTargetConfig, targetMap);
    ASSERT_OK(storageTargets.load(executor));
    auto targetResult = targetMap.snapshot()->getTarget(targetId);
    ASSERT_OK(targetResult);
    auto storageTarget = (*targetResult)->storageTarget;

    ASSERT_EQ(storageTargets.fds().size(), 8 * bool(loop) + 8);

    UpdateWorker::Config updateWorkerConfig;
    UpdateWorker updateWorker(updateWorkerConfig);
    ASSERT_OK(updateWorker.start(1));

    AioReadWorker::Config aioReadWorkerConfig;
    AioReadWorker aioReadWorker(aioReadWorkerConfig);
    ASSERT_OK(aioReadWorker.start(storageTargets.fds(), {}));

    auto chunkSize = loop == 0 ? 1_MB : Size{256_KB * loop};
    ASSERT_OK(storageTarget->addChunkSize({chunkSize}));
    ASSERT_OK(storageTarget->addChunkSize({chunkSize}));

    ChunkEngineUpdateJob updateChunk{};

    // write data to chunk.
    {
      UpdateIO writeIO;
      writeIO.key.chunkId = chunkId;
      writeIO.chunkSize = chunkSize;
      writeIO.length = chunkSize;
      writeIO.offset = 0;
      writeIO.updateVer = ChunkVer{1};
      writeIO.updateType = UpdateType::WRITE;

      UpdateJob updateJob(requestCtx, writeIO, {}, updateChunk, storageTarget);
      updateJob.state().data = reinterpret_cast<const uint8_t *>(dataBytes.data());

      folly::coro::blockingWait(updateWorker.enqueue(&updateJob));
      folly::coro::blockingWait(updateJob.complete());
      ASSERT_OK(updateJob.result().lengthInfo);
      ASSERT_EQ(updateJob.result().lengthInfo.value(), chunkSize);
      ASSERT_EQ(updateJob.result().commitVer, ChunkVer{0});
      ASSERT_EQ(updateJob.result().updateVer, ChunkVer{1});
    }

    ASSERT_EQ(storageTarget->usedSize(), chunkSize);

    // aio read uncommit chunk.
    {
      std::string buf(chunkSize, '\0');
      BatchReadReq req;
      req.payloads.emplace_back();
      auto &aio = req.payloads.front();
      aio.key.chunkId = chunkId;
      aio.length = chunkSize;
      aio.offset = 0;
      // aio.addr = ...;

      BatchReadRsp rsp;
      rsp.results.resize(1);
      auto job = std::make_unique<BatchReadJob>(req.payloads, rsp.results, ChecksumType::NONE);
      job->front().state().storageTarget = storageTarget.get();
      ASSERT_EQ(storageTarget->aioPrepareRead(job->front()).error().code(), StorageCode::kChunkNotCommit);
    }

    // commit chunk.
    {
      CommitIO commitIO;
      commitIO.key.chunkId = chunkId;
      commitIO.commitVer = ChunkVer{1};

      auto updateJob = UpdateJob(requestCtx, commitIO, {}, updateChunk, storageTarget);
      updateJob.state().data = reinterpret_cast<const uint8_t *>(dataBytes.data());

      folly::coro::blockingWait(updateWorker.enqueue(&updateJob));
      folly::coro::blockingWait(updateJob.complete());
      ASSERT_OK(updateJob.result().lengthInfo);
      ASSERT_EQ(updateJob.result().commitVer, ChunkVer{1});
      ASSERT_EQ(updateJob.result().updateVer, ChunkVer{1});
    }

    // remove chunk.
    {
      UpdateIO removeIO;
      removeIO.key.chunkId = chunkId;
      removeIO.updateType = UpdateType::REMOVE;

      auto updateJob = UpdateJob(requestCtx, removeIO, {}, updateChunk, storageTarget);

      folly::coro::blockingWait(updateWorker.enqueue(&updateJob));
      folly::coro::blockingWait(updateJob.complete());
      ASSERT_OK(updateJob.result().lengthInfo);
      ASSERT_EQ(updateJob.result().commitVer, ChunkVer{1});
      ASSERT_EQ(updateJob.result().updateVer, ChunkVer{2});
    }

    ASSERT_EQ(storageTarget->usedSize(), chunkSize);

    // commit remove.
    {
      CommitIO commitIO;
      commitIO.key.chunkId = chunkId;
      commitIO.commitVer = ChunkVer{2};
      auto updateJob = UpdateJob(requestCtx, commitIO, {}, updateChunk, storageTarget);

      folly::coro::blockingWait(updateWorker.enqueue(&updateJob));
      folly::coro::blockingWait(updateJob.complete());
      ASSERT_OK(updateJob.result().lengthInfo);
      ASSERT_EQ(updateJob.result().commitVer, ChunkVer{2});
      ASSERT_EQ(updateJob.result().updateVer, ChunkVer{2});
    }

    // remove non-existent chunk.
    {
      UpdateIO removeIO;
      removeIO.key.chunkId = chunkId;
      removeIO.updateType = UpdateType::REMOVE;

      auto updateJob = UpdateJob(requestCtx, removeIO, {}, updateChunk, storageTarget);

      folly::coro::blockingWait(updateWorker.enqueue(&updateJob));
      folly::coro::blockingWait(updateJob.complete());
      ASSERT_OK(updateJob.result().lengthInfo);
    }

    // create new target.
    {
      CreateTargetReq req;
      req.targetId = TargetId{7};
      req.physicalFileCount = 8;
      req.chunkSizeList = {1_MB};
      req.allowExistingTarget = false;
      req.chainId = ChainId{1};
      if (loop == 0) {
        ASSERT_OK(storageTargets.create(req));
      } else {
        ASSERT_FALSE(storageTargets.create(req));
      }
    }

    ASSERT_EQ(storageTarget->usedSize(), 0);
  }
}

TEST(TestLocalTargetStateManager, UpdateLocalState) {
  using LS = enum hf3fs::flat::LocalTargetState;
  using PS = enum hf3fs::flat::PublicTargetState;

  ASSERT_EQ(TargetMap::updateLocalState(TargetId{0}, LS::ONLINE, PS::SERVING), LS::UPTODATE);
  ASSERT_EQ(TargetMap::updateLocalState(TargetId{0}, LS::ONLINE, PS::LASTSRV), LS::ONLINE);
  ASSERT_EQ(TargetMap::updateLocalState(TargetId{0}, LS::ONLINE, PS::SYNCING), LS::ONLINE);
  ASSERT_EQ(TargetMap::updateLocalState(TargetId{0}, LS::ONLINE, PS::WAITING), LS::ONLINE);
  ASSERT_EQ(TargetMap::updateLocalState(TargetId{0}, LS::ONLINE, PS::OFFLINE), LS::ONLINE);

  ASSERT_EQ(TargetMap::updateLocalState(TargetId{0}, LS::UPTODATE, PS::SERVING), LS::UPTODATE);
  ASSERT_EQ(TargetMap::updateLocalState(TargetId{0}, LS::UPTODATE, PS::LASTSRV), LS::OFFLINE);
  ASSERT_EQ(TargetMap::updateLocalState(TargetId{0}, LS::UPTODATE, PS::SYNCING), LS::UPTODATE);
  ASSERT_EQ(TargetMap::updateLocalState(TargetId{0}, LS::UPTODATE, PS::WAITING), LS::OFFLINE);
  ASSERT_EQ(TargetMap::updateLocalState(TargetId{0}, LS::UPTODATE, PS::OFFLINE), LS::OFFLINE);
}

TEST(TestStorageTarget, Uncommitted) {
  folly::test::TemporaryDirectory tmpPath;
  CPUExecutorGroup executor(8, "");

  const auto chunkId = ChunkId{12345, 12345};
  constexpr TargetId targetId{0};
  constexpr auto chunkSize = 1_MB;
  std::string dataBytes(chunkSize, 'B');
  ServiceRequestContext requestCtx;

  StorageTargets::Config storageTargetConfig;
  storageTargetConfig.set_target_num_per_path(1);
  storageTargetConfig.set_target_paths({tmpPath.path()});
  storageTargetConfig.set_allow_disk_without_uuid(true);

  ChunkEngineUpdateJob updateChunk{};

  {
    StorageTargets::CreateConfig createConfig;
    createConfig.set_chunk_size_list({1_MB});
    createConfig.set_physical_file_count(8);
    createConfig.set_allow_disk_without_uuid(true);
    createConfig.set_target_ids({targetId});
    createConfig.set_only_chunk_engine(true);

    AtomicallyTargetMap targetMap;
    StorageTargets targets(storageTargetConfig, targetMap);
    ASSERT_OK(targets.create(createConfig));

    auto targetResult = targetMap.snapshot()->getTarget(targetId);
    ASSERT_OK(targetResult);
    auto storageTarget = (*targetResult)->storageTarget;

    UpdateWorker::Config updateWorkerConfig;
    UpdateWorker updateWorker(updateWorkerConfig);
    ASSERT_OK(updateWorker.start(1));

    // write data to chunk.
    UpdateIO writeIO;
    writeIO.key.chunkId = chunkId;
    writeIO.chunkSize = chunkSize;
    writeIO.length = chunkSize;
    writeIO.offset = 0;
    writeIO.updateVer = ChunkVer{1};
    writeIO.updateType = UpdateType::WRITE;

    UpdateJob updateJob(requestCtx, writeIO, {}, updateChunk, storageTarget);
    updateJob.state().data = reinterpret_cast<const uint8_t *>(dataBytes.data());

    folly::coro::blockingWait(updateWorker.enqueue(&updateJob));
    folly::coro::blockingWait(updateJob.complete());
    ASSERT_OK(updateJob.result().lengthInfo);
    ASSERT_EQ(updateJob.result().lengthInfo.value(), chunkSize);
    // ASSERT_EQ(updateJob.result().commitVer, ChunkVer{0});
    ASSERT_EQ(updateJob.result().updateVer, ChunkVer{1});

    // commit it.
    CommitIO commitIO;
    commitIO.commitVer = ChunkVer{1};
    commitIO.key.chunkId = chunkId;
    UpdateJob commitJob(requestCtx, commitIO, {}, updateChunk, storageTarget);
    folly::coro::blockingWait(updateWorker.enqueue(&commitJob));
    folly::coro::blockingWait(commitJob.complete());
    ASSERT_OK(commitJob.result().lengthInfo);
    ASSERT_EQ(commitJob.result().commitVer, ChunkVer{1});
    ASSERT_EQ(commitJob.result().updateVer, ChunkVer{1});

    // write again.
    writeIO.updateVer = ChunkVer{2};
    UpdateJob updateJob2(requestCtx, writeIO, {}, updateChunk, storageTarget);
    updateJob2.state().data = reinterpret_cast<const uint8_t *>(dataBytes.data());
    folly::coro::blockingWait(updateWorker.enqueue(&updateJob2));
    folly::coro::blockingWait(updateJob2.complete());
    ASSERT_OK(updateJob2.result().lengthInfo);
    ASSERT_EQ(updateJob2.result().lengthInfo.value(), chunkSize);
    // ASSERT_EQ(updateJob2.result().commitVer, ChunkVer{1});
    ASSERT_EQ(updateJob2.result().updateVer, ChunkVer{2});
  }

  {
    AtomicallyTargetMap targetMap;
    StorageTargets storageTargets(storageTargetConfig, targetMap);
    ASSERT_OK(storageTargets.load(executor));

    auto targetResult = targetMap.snapshot()->getTarget(targetId);
    ASSERT_OK(targetResult);
    auto storageTarget = (*targetResult)->storageTarget;

    auto uncommitted = storageTarget->uncommitted().value();
    ASSERT_EQ(uncommitted.size(), 1);
    ASSERT_EQ(uncommitted.front(), chunkId);

    ASSERT_OK(storageTarget->resetUncommitted(ChainVer{1}));

    auto metaResult = storageTarget->queryChunk(chunkId);
    ASSERT_OK(metaResult);
    // ASSERT_EQ(metaResult->lastClientUuid, Uuid::max());
  }

  {
    AtomicallyTargetMap targetMap;
    StorageTargets storageTargets(storageTargetConfig, targetMap);
    ASSERT_OK(storageTargets.load(executor));

    auto targetResult = targetMap.snapshot()->getTarget(targetId);
    ASSERT_OK(targetResult);
    auto storageTarget = (*targetResult)->storageTarget;

    auto uncommitted = storageTarget->uncommitted().value();
    ASSERT_TRUE(uncommitted.empty());
  }
}

TEST(TestStorageTarget, UnRecycle) {
  folly::test::TemporaryDirectory tmpPath;

  const auto chunkId = ChunkId{12345, 12345};
  constexpr TargetId targetId{0};
  constexpr auto chunkSize = 1_MB;
  std::string dataBytes(chunkSize, 'B');
  ServiceRequestContext requestCtx;

  StorageTargets::Config storageTargetConfig;
  storageTargetConfig.storage_target().meta_store().set_removed_chunk_expiration_time(0_s);
  storageTargetConfig.set_target_num_per_path(1);
  storageTargetConfig.set_target_paths({tmpPath.path()});
  storageTargetConfig.set_allow_disk_without_uuid(true);

  StorageTargets::CreateConfig createConfig;
  createConfig.set_chunk_size_list({1_MB});
  createConfig.set_physical_file_count(8);
  createConfig.set_allow_disk_without_uuid(true);
  createConfig.set_target_ids({targetId});

  AtomicallyTargetMap targetMap;
  StorageTargets targets(storageTargetConfig, targetMap);
  ASSERT_OK(targets.create(createConfig));

  auto targetResult = targetMap.snapshot()->getTarget(targetId);
  ASSERT_OK(targetResult);
  auto storageTarget = (*targetResult)->storageTarget;

  UpdateWorker::Config updateWorkerConfig;
  UpdateWorker updateWorker(updateWorkerConfig);
  ASSERT_OK(updateWorker.start(1));

  ChunkEngineUpdateJob updateChunk{};

  // 1. write data to chunk.
  {
    UpdateIO writeIO;
    writeIO.key.chunkId = chunkId;
    writeIO.chunkSize = chunkSize;
    writeIO.length = chunkSize;
    writeIO.offset = 0;
    writeIO.updateVer = ChunkVer{1};
    writeIO.updateType = UpdateType::WRITE;

    UpdateJob updateJob(requestCtx, writeIO, {}, updateChunk, storageTarget);
    updateJob.state().data = reinterpret_cast<const uint8_t *>(dataBytes.data());

    folly::coro::blockingWait(updateWorker.enqueue(&updateJob));
    folly::coro::blockingWait(updateJob.complete());
    ASSERT_OK(updateJob.result().lengthInfo);
    ASSERT_EQ(updateJob.result().lengthInfo.value(), chunkSize);
    ASSERT_EQ(updateJob.result().commitVer, ChunkVer{0});
    ASSERT_EQ(updateJob.result().updateVer, ChunkVer{1});

    CommitIO commitIO;
    commitIO.commitVer = ChunkVer{1};
    commitIO.key.chunkId = chunkId;
    UpdateJob commitJob(requestCtx, commitIO, {}, updateChunk, storageTarget);
    folly::coro::blockingWait(updateWorker.enqueue(&commitJob));
    folly::coro::blockingWait(commitJob.complete());
    ASSERT_OK(commitJob.result().lengthInfo);
    ASSERT_EQ(commitJob.result().commitVer, ChunkVer{1});
    ASSERT_EQ(commitJob.result().updateVer, ChunkVer{1});
  }

  auto metaResult = storageTarget->queryChunk(chunkId);
  ASSERT_OK(metaResult);
  auto recycleResult = storageTarget->punchHole();
  ASSERT_OK(recycleResult);
  ASSERT_EQ(*recycleResult, true);

  // 2. remove this chunk.
  {
    UpdateIO writeIO;
    writeIO.key.chunkId = chunkId;
    writeIO.chunkSize = chunkSize;
    writeIO.length = chunkSize;
    writeIO.offset = 0;
    writeIO.updateVer = ChunkVer{2};
    writeIO.updateType = UpdateType::REMOVE;

    UpdateJob updateJob(requestCtx, writeIO, {}, updateChunk, storageTarget);
    updateJob.state().data = reinterpret_cast<const uint8_t *>(dataBytes.data());

    folly::coro::blockingWait(updateWorker.enqueue(&updateJob));
    folly::coro::blockingWait(updateJob.complete());
    ASSERT_OK(updateJob.result().lengthInfo);
    ASSERT_EQ(updateJob.result().commitVer, ChunkVer{1});
    ASSERT_EQ(updateJob.result().updateVer, ChunkVer{2});

    CommitIO commitIO;
    commitIO.commitVer = ChunkVer{2};
    commitIO.key.chunkId = chunkId;
    UpdateJob commitJob(requestCtx, commitIO, {}, updateChunk, storageTarget);
    folly::coro::blockingWait(updateWorker.enqueue(&commitJob));
    folly::coro::blockingWait(commitJob.complete());
    ASSERT_OK(commitJob.result().lengthInfo);
    ASSERT_EQ(commitJob.result().commitVer, ChunkVer{2});
    ASSERT_EQ(commitJob.result().updateVer, ChunkVer{2});
  }

  recycleResult = storageTarget->punchHole();
  ASSERT_OK(recycleResult);
  ASSERT_EQ(*recycleResult, true);
}

TEST(TestStorageTarget, Migrate) {
  constexpr auto N = 16u;
  folly::test::TemporaryDirectory tmpPath;
  CPUExecutorGroup executor(8, "");

  const auto chunkId = ChunkId{12345, 12345};
  constexpr TargetId targetId{0};
  constexpr auto chunkSize = 1_MB;
  std::string dataBytes(chunkSize, 'B');
  ServiceRequestContext requestCtx;

  StorageTargets::Config storageTargetConfig;
  storageTargetConfig.set_target_num_per_path(1);
  storageTargetConfig.set_target_paths({tmpPath.path()});
  storageTargetConfig.storage_target().kv_store().set_type(kv::KVStore::Type::LevelDB);
  storageTargetConfig.set_allow_disk_without_uuid(true);

  ChunkEngineUpdateJob updateChunk{};

  // 1. create with LevelDB.
  {
    StorageTargets::CreateConfig createConfig;
    createConfig.set_chunk_size_list({1_MB});
    createConfig.set_physical_file_count(8);
    createConfig.set_allow_disk_without_uuid(true);
    createConfig.set_target_ids({targetId});

    AtomicallyTargetMap targetMap;
    StorageTargets targets(storageTargetConfig, targetMap);
    ASSERT_OK(targets.create(createConfig));
  }

  // 2. write some chunks.
  {
    AtomicallyTargetMap targetMap;
    StorageTargets storageTargets(storageTargetConfig, targetMap);
    ASSERT_OK(storageTargets.load(executor));
    auto targetResult = targetMap.snapshot()->getTarget(targetId);
    ASSERT_OK(targetResult);
    auto storageTarget = (*targetResult)->storageTarget;

    UpdateWorker::Config updateWorkerConfig;
    UpdateWorker updateWorker(updateWorkerConfig);
    ASSERT_OK(updateWorker.start(1));

    // write data to chunk.
    for (auto i = 0u; i < N; ++i) {
      UpdateIO writeIO;
      writeIO.key.chunkId = ChunkId{0, i};
      writeIO.chunkSize = chunkSize;
      writeIO.length = chunkSize;
      writeIO.offset = 0;
      writeIO.updateVer = ChunkVer{1};
      writeIO.updateType = UpdateType::WRITE;

      UpdateJob updateJob(requestCtx, writeIO, {}, updateChunk, storageTarget);
      updateJob.state().data = reinterpret_cast<const uint8_t *>(dataBytes.data());

      folly::coro::blockingWait(updateWorker.enqueue(&updateJob));
      folly::coro::blockingWait(updateJob.complete());
      ASSERT_OK(updateJob.result().lengthInfo);
      ASSERT_EQ(updateJob.result().lengthInfo.value(), chunkSize);
      ASSERT_EQ(updateJob.result().commitVer, ChunkVer{0});
      ASSERT_EQ(updateJob.result().updateVer, ChunkVer{1});
    }

    for (auto i = 0u; i < N; ++i) {
      CommitIO commitIO;
      commitIO.key.chunkId = ChunkId{0, i};
      commitIO.commitVer = ChunkVer{1};

      auto updateJob = UpdateJob(requestCtx, commitIO, {}, updateChunk, storageTarget);
      updateJob.state().data = reinterpret_cast<const uint8_t *>(dataBytes.data());

      folly::coro::blockingWait(updateWorker.enqueue(&updateJob));
      folly::coro::blockingWait(updateJob.complete());
      ASSERT_OK(updateJob.result().lengthInfo);
      ASSERT_EQ(updateJob.result().commitVer, ChunkVer{1});
      ASSERT_EQ(updateJob.result().updateVer, ChunkVer{1});
    }

    for (auto i = 0u; i < N; ++i) {
      auto metaResult = storageTarget->queryChunk(ChunkId{0, i});
      ASSERT_OK(metaResult);
    }
  }

  // 3. migrate.
  storageTargetConfig.storage_target().set_migrate_kv_store(true);
  storageTargetConfig.storage_target().kv_store().set_type(kv::KVStore::Type::RocksDB);
  {
    AtomicallyTargetMap targetMap;
    StorageTargets storageTargets(storageTargetConfig, targetMap);
    ASSERT_OK(storageTargets.load(executor));
    auto targetResult = targetMap.snapshot()->getTarget(targetId);
    ASSERT_OK(targetResult);
    auto storageTarget = (*targetResult)->storageTarget;

    for (auto i = 0u; i < N; ++i) {
      auto metaResult = storageTarget->queryChunk(ChunkId{0, i});
      ASSERT_OK(metaResult);
    }
  }
}

}  // namespace
}  // namespace hf3fs::storage
