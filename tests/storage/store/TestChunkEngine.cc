#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/hash/Checksum.h>
#include <folly/logging/xlog.h>

#include "chunk_engine/src/cxx.rs.h"
#include "common/utils/Size.h"
#include "fbs/storage/Common.h"
#include "storage/aio/AioReadWorker.h"
#include "storage/aio/BatchReadJob.h"
#include "storage/service/BufferPool.h"
#include "storage/service/TargetMap.h"
#include "storage/store/ChunkStore.h"
#include "storage/store/StorageTarget.h"
#include "storage/update/UpdateJob.h"
#include "storage/update/UpdateWorker.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::storage::test {
namespace {

TEST(TestChunkEngine, ReadWrite) {
  folly::test::TemporaryDirectory tmpPath;
  CPUExecutorGroup executor(8, "");

  constexpr TargetId targetId{1};
  constexpr auto chunkSize = 512_KB;
  std::string dataBytes(chunkSize, 'B');
  ServiceRequestContext requestCtx;

  StorageTargets::Config storageTargetConfig;
  storageTargetConfig.set_target_num_per_path(1);
  storageTargetConfig.set_target_paths({tmpPath.path()});
  storageTargetConfig.storage_target().file_store().set_preopen_chunk_size_list({chunkSize});
  storageTargetConfig.set_allow_disk_without_uuid(true);

  {
    StorageTargets::CreateConfig createConfig;
    createConfig.set_chunk_size_list({chunkSize});
    createConfig.set_physical_file_count(8);
    createConfig.set_allow_disk_without_uuid(true);
    createConfig.set_target_ids({targetId});

    AtomicallyTargetMap targetMap;
    StorageTargets targets(storageTargetConfig, targetMap);
    ASSERT_OK(targets.create(createConfig));
  }

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

    BufferPool::Config bufferPoolConfig;
    bufferPoolConfig.set_rdmabuf_count(4);
    bufferPoolConfig.set_big_rdmabuf_count(1);
    BufferPool pool(bufferPoolConfig);
    ASSERT_OK(pool.init(executor));

    AioReadWorker::Config aioReadWorkerConfig;
    AioReadWorker aioReadWorker(aioReadWorkerConfig);
    ASSERT_OK(aioReadWorker.start(storageTargets.fds(), pool.iovecs()));

    const auto chunkEngine = 1ul << 40;
    for (auto high : {0ul, chunkEngine}) {
      auto chunkId = ChunkId(high, 1);

      // aio read.
      {
        BatchReadReq req;
        req.payloads.emplace_back();
        auto &aio = req.payloads.front();
        aio.key.chunkId = chunkId;
        aio.length = chunkSize;
        aio.offset = 0;

        BatchReadRsp rsp;
        rsp.results.resize(1);
        auto job = std::make_unique<BatchReadJob>(req.payloads, rsp.results, ChecksumType::NONE);
        job->front().state().storageTarget = storageTarget.get();

        ASSERT_EQ(storageTarget->aioPrepareRead(job->front()).error().code(), StorageCode::kChunkMetadataNotFound);
      }

      ChunkEngineUpdateJob updateChunk{};

      // write a chunk.
      {
        UpdateIO writeIO;
        writeIO.key.chunkId = chunkId;
        writeIO.chunkSize = chunkSize;
        writeIO.length = chunkSize;
        writeIO.offset = 0;
        writeIO.updateVer = ChunkVer{1};
        writeIO.updateType = UpdateType::WRITE;

        auto data = reinterpret_cast<const uint8_t *>(dataBytes.data());
        writeIO.checksum = ChecksumInfo::create(ChecksumType::CRC32C, data, chunkSize);

        UpdateJob updateJob(requestCtx, writeIO, {}, updateChunk, storageTarget);
        updateJob.state().data = data;

        folly::coro::blockingWait(updateWorker.enqueue(&updateJob));
        folly::coro::blockingWait(updateJob.complete());
        ASSERT_OK(updateJob.result().lengthInfo);
        ASSERT_EQ(updateJob.result().lengthInfo.value(), chunkSize);
      }

      // aio read.
      {
        BatchReadReq req;
        req.payloads.emplace_back();
        auto &aio = req.payloads.front();
        aio.key.chunkId = chunkId;
        aio.length = chunkSize;
        aio.offset = 0;

        BatchReadRsp rsp;
        rsp.results.resize(1);
        auto job = std::make_unique<BatchReadJob>(req.payloads, rsp.results, ChecksumType::NONE);
        job->front().state().storageTarget = storageTarget.get();

        ASSERT_TRUE(storageTarget->aioPrepareRead(job->front()).hasError());
      }

      // commit a chunk.
      {
        CommitIO commitIO;
        commitIO.key.chunkId = chunkId;
        commitIO.commitVer = ChunkVer{1};

        UpdateJob commitJob(requestCtx, commitIO, {}, updateChunk, storageTarget);

        folly::coro::blockingWait(updateWorker.enqueue(&commitJob));
        folly::coro::blockingWait(commitJob.complete());
        ASSERT_OK(commitJob.result().lengthInfo);
      }

      // aio read again.
      {
        BatchReadReq req;
        req.payloads.emplace_back();
        auto &aio = req.payloads.front();
        aio.key.chunkId = chunkId;
        aio.length = chunkSize;
        aio.offset = 0;

        BatchReadRsp rsp;
        rsp.results.resize(1);
        auto job = std::make_unique<BatchReadJob>(req.payloads, rsp.results, ChecksumType::NONE);
        job->front().state().storageTarget = storageTarget.get();
        auto buffer = pool.get();
        job->front().state().localbuf = buffer.tryAllocate(chunkSize).value();

        ASSERT_TRUE(storageTarget->aioPrepareRead(job->front()).hasValue());

        folly::coro::blockingWait(aioReadWorker.enqueue(AioReadJobIterator(job.get())));
        folly::coro::blockingWait(job->complete());
        ASSERT_TRUE(job->front().result().lengthInfo);
        ASSERT_EQ(job->front().result().lengthInfo.value(), chunkSize);
        std::string_view out{(const char *)job->front().state().localbuf.ptr(), chunkSize};
        ASSERT_EQ(out, dataBytes);
      }

      // query chunks.
      {
        ChunkIdRange chunkIdRange;
        chunkIdRange.begin = ChunkId(high, 0);
        chunkIdRange.end = ChunkId(high + 1, 0);
        chunkIdRange.maxNumChunkIdsToProcess = 128;
        auto chunks = storageTarget->queryChunks(chunkIdRange);
        ASSERT_TRUE(chunks);
        ASSERT_EQ(chunks->size(), 1);
        ASSERT_EQ(chunks->front().first, chunkId);
        auto &meta = chunks->front().second;
        ASSERT_EQ(meta.size, chunkSize);

        auto result = storageTarget->queryChunk(chunkId);
        ASSERT_TRUE(result);
        ASSERT_EQ(meta, *result);
      }

      // query chunks (optimize).
      {
        ChunkIdRange chunkIdRange;
        chunkIdRange.begin = ChunkId(high, 1);
        chunkIdRange.end = chunkIdRange.begin.nextChunkId();
        chunkIdRange.maxNumChunkIdsToProcess = 128;
        auto chunks = storageTarget->queryChunks(chunkIdRange);
        ASSERT_TRUE(chunks);
        ASSERT_EQ(chunks->size(), 1);
        ASSERT_EQ(chunks->front().first, chunkId);
        auto &meta = chunks->front().second;
        ASSERT_EQ(meta.size, chunkSize);

        auto result = storageTarget->queryChunk(chunkId);
        ASSERT_TRUE(result);
        ASSERT_EQ(meta, *result);
      }
    }

    ChunkMetaVector metadataVec;
    ASSERT_TRUE(storageTarget->getAllMetadata(metadataVec));
    ASSERT_EQ(metadataVec.size(), 2);
    ASSERT_GT(metadataVec.front().chunkId, metadataVec.back().chunkId);
  }
}

}  // namespace
}  // namespace hf3fs::storage::test
