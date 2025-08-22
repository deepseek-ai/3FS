#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/net/Client.h"
#include "tests/GtestHelpers.h"
#include "tests/lib/UnitTestFabric.h"

namespace hf3fs::storage::client {
namespace {

using namespace hf3fs::test;

struct ClientTestConfig {
  hf3fs::Duration initWaitTime;
  hf3fs::Duration maxWaitTime;
  hf3fs::Duration maxRetryTime;
  size_t batchSize;
  size_t maxBatchSize;
  size_t numConcurrentReqs;
  size_t maxConcurrentReqs;
};

// high-concurrency stress test
class TestStorageClientHCStress : public UnitTestFabric, public ::testing::TestWithParam<ClientTestConfig> {
 protected:
  TestStorageClientHCStress()
      : UnitTestFabric(SystemSetupConfig{128_KB /*chunkSize*/,
                                         3 /*numChains*/,
                                         2 /*numReplicas*/,
                                         3 /*numStorageNodes*/,
                                         {folly::fs::temp_directory_path()}}) {}

  void SetUp() override {
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    GTEST_SKIP() << "Skipping all tests since thread sanitizer enabled";
#endif
#endif

    // init ib device
    net::IBDevice::Config ibConfig;
    auto ibResult = net::IBManager::start(ibConfig);
    ASSERT_OK(ibResult);

    auto testConfig = GetParam();

    clientConfig_.traffic_control().read().set_max_batch_size(testConfig.maxBatchSize);
    clientConfig_.traffic_control().write().set_max_batch_size(testConfig.maxBatchSize);
    clientConfig_.traffic_control().remove().set_max_batch_size(testConfig.maxBatchSize);
    clientConfig_.traffic_control().truncate().set_max_batch_size(testConfig.maxBatchSize);
    clientConfig_.traffic_control().read().set_max_concurrent_requests(testConfig.maxConcurrentReqs);
    clientConfig_.traffic_control().write().set_max_concurrent_requests(testConfig.maxConcurrentReqs);
    clientConfig_.traffic_control().remove().set_max_concurrent_requests(testConfig.maxConcurrentReqs);
    clientConfig_.traffic_control().truncate().set_max_concurrent_requests(testConfig.maxConcurrentReqs);

    batchSize_ = testConfig.batchSize;
    numConcurrentReqs_ = testConfig.numConcurrentReqs;
    numChunks_ = batchSize_ * numConcurrentReqs_;

    ASSERT_TRUE(setUpStorageSystem());

    // increase timeout to avoid comm error during data generation
    clientConfig_.retry().set_init_wait_time(3_s);
    clientConfig_.retry().set_max_wait_time(10_s);
    clientConfig_.retry().set_max_retry_time(300_s);

    // create chunks
    std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);
    for (auto chainId : chainIds_) {
      auto result = writeToChunks(chainId, ChunkId(1, 0), ChunkId(1, numChunks_), chunkData);
      ASSERT_TRUE(result);
      XLOGF(INFO, "Created {} test chunks on chain {}", numChunks_, chainId);
    }

    // set timeout for test
    clientConfig_.retry().set_init_wait_time(testConfig.initWaitTime);
    clientConfig_.retry().set_max_wait_time(testConfig.maxWaitTime);
    clientConfig_.retry().set_max_retry_time(testConfig.maxRetryTime);
  }

  void TearDown() override { tearDownStorageSystem(); }

 protected:
  size_t numConcurrentReqs_;
  size_t numChunks_;
  size_t batchSize_;
};

TEST_P(TestStorageClientHCStress, BatchRead) {
  // register a block of memory
  std::vector<uint8_t> memoryBlock(setupConfig_.chunk_size() * numChunks_, 0);
  auto regRes = storageClient_->registerIOBuffer(&memoryBlock[0], memoryBlock.size());
  ASSERT_OK(regRes);
  auto ioBuffer = std::move(*regRes);

  // create read IOs

  std::vector<ReadIO> readIOs;
  size_t chunkIndex = 0;

  for (size_t reqIndex = 0; reqIndex < numConcurrentReqs_; reqIndex++) {
    for (size_t ioIndex = 0; ioIndex < batchSize_; ioIndex++, chunkIndex++) {
      ChunkId chunkId(1 /*high*/, chunkIndex /*low*/);
      ChainId chainId = chainIds_[chunkIndex % chainIds_.size()];
      auto readIO = storageClient_->createReadIO(chainId,
                                                 chunkId,
                                                 0 /*offset*/,
                                                 setupConfig_.chunk_size() /*length*/,
                                                 &memoryBlock[chunkIndex * setupConfig_.chunk_size()],
                                                 &ioBuffer);
      readIOs.push_back(std::move(readIO));
    }
  }

  flat::UserInfo dummyUserInfo{};
  ReadOptions options;
  std::vector<folly::SemiFuture<folly::Expected<folly::Unit, hf3fs::Status>>> ioTasks;

  // start many coroutines to hit the concurrency limit
  for (size_t reqIndex = 0; reqIndex < numConcurrentReqs_; reqIndex++) {
    auto ios = std::span<ReadIO>(&readIOs[batchSize_ * reqIndex], &readIOs[batchSize_ * (reqIndex + 1)]);
    auto task = storageClient_->batchRead(ios, dummyUserInfo, options).scheduleOn(&requestExe_).start();
    ioTasks.push_back(std::move(task));
  }

  folly::coro::blockingWait(folly::coro::collectAllRange(std::move(ioTasks)));

  for (const auto &readIO : readIOs) {
    ASSERT_OK(readIO.result.lengthInfo);
    ASSERT_EQ(readIO.length, readIO.result.lengthInfo.value());
    ASSERT_EQ(1, readIO.result.commitVer);
  }

  for (unsigned char &byte : memoryBlock) ASSERT_EQ(0xFF, byte);
}

TEST_P(TestStorageClientHCStress, BatchWrite) {
  // register a block of memory
  std::vector<uint8_t> memoryBlock(setupConfig_.chunk_size() * numChunks_, 0xFF);
  auto regRes = storageClient_->registerIOBuffer(&memoryBlock[0], memoryBlock.size());
  ASSERT_OK(regRes);
  auto ioBuffer = std::move(*regRes);

  // create write IOs

  std::vector<WriteIO> writeIOs;
  size_t chunkIndex = 0;

  for (size_t reqIndex = 0; reqIndex < numConcurrentReqs_; reqIndex++) {
    for (size_t ioIndex = 0; ioIndex < batchSize_; ioIndex++, chunkIndex++) {
      ChunkId chunkId(1 /*high*/, chunkIndex /*low*/);
      ChainId chainId = chainIds_[chunkIndex % chainIds_.size()];
      auto writeIO = storageClient_->createWriteIO(chainId,
                                                   chunkId,
                                                   0 /*offset*/,
                                                   setupConfig_.chunk_size() /*length*/,
                                                   setupConfig_.chunk_size() /*chunkSize*/,
                                                   &memoryBlock[chunkIndex * setupConfig_.chunk_size()],
                                                   &ioBuffer);
      writeIOs.push_back(std::move(writeIO));
    }
  }

  flat::UserInfo dummyUserInfo{};
  WriteOptions options;
  std::vector<folly::SemiFuture<folly::Expected<folly::Unit, hf3fs::Status>>> ioTasks;

  // start many coroutines to hit the concurrency limit
  for (size_t reqIndex = 0; reqIndex < numConcurrentReqs_; reqIndex++) {
    auto ios = std::span<WriteIO>(&writeIOs[batchSize_ * reqIndex], &writeIOs[batchSize_ * (reqIndex + 1)]);
    auto task = storageClient_->batchWrite(ios, dummyUserInfo, options).scheduleOn(&requestExe_).start();
    ioTasks.push_back(std::move(task));
  }

  folly::coro::blockingWait(folly::coro::collectAllRange(std::move(ioTasks)));

  for (const auto &writeIO : writeIOs) {
    ASSERT_OK(writeIO.result.lengthInfo);
    ASSERT_EQ(writeIO.length, writeIO.result.lengthInfo.value());
    ASSERT_EQ(2, writeIO.result.commitVer);
  }
}

TEST_P(TestStorageClientHCStress, BatchTruncate) {
  // create truncate ops

  std::vector<TruncateChunkOp> truncateOps;
  size_t chunkIndex = 0;

  for (size_t reqIndex = 0; reqIndex < numConcurrentReqs_; reqIndex++) {
    for (size_t ioIndex = 0; ioIndex < batchSize_; ioIndex++, chunkIndex++) {
      ChunkId chunkId(1 /*high*/, chunkIndex /*low*/);
      ChainId chainId = chainIds_[chunkIndex % chainIds_.size()];
      auto truncateOp = storageClient_->createTruncateOp(chainId,
                                                         chunkId,
                                                         setupConfig_.chunk_size() / 2 /*length*/,
                                                         setupConfig_.chunk_size() /*chunkSize*/);
      truncateOps.push_back(std::move(truncateOp));
    }
  }

  flat::UserInfo dummyUserInfo{};
  WriteOptions options;
  std::vector<folly::SemiFuture<folly::Expected<folly::Unit, hf3fs::Status>>> ioTasks;

  // start many coroutines to hit the concurrency limit and use all channel ids
  for (size_t reqIndex = 0; reqIndex < numConcurrentReqs_; reqIndex++) {
    auto ops =
        std::span<TruncateChunkOp>(&truncateOps[batchSize_ * reqIndex], &truncateOps[batchSize_ * (reqIndex + 1)]);
    auto task = storageClient_->truncateChunks(ops, dummyUserInfo, options).scheduleOn(&requestExe_).start();
    ioTasks.push_back(std::move(task));
  }

  folly::coro::blockingWait(folly::coro::collectAllRange(std::move(ioTasks)));

  for (const auto &truncateOp : truncateOps) {
    XLOGF(DBG5, "Truncate chunk {} to length {}/{}", truncateOp.chunkId, truncateOp.chunkLen, truncateOp.chunkSize);
    ASSERT_OK(truncateOp.result.lengthInfo);
    ASSERT_EQ(truncateOp.chunkLen, truncateOp.result.lengthInfo.value());
    ASSERT_EQ(2, truncateOp.result.commitVer);
  }
}

TEST_P(TestStorageClientHCStress, BatchRemove) {
  // create remove ops

  std::vector<RemoveChunksOp> removeOps;

  for (size_t reqIndex = 0; reqIndex < numConcurrentReqs_; reqIndex++) {
    ChunkId chunkBegin(1 /*high*/, batchSize_ * reqIndex /*low*/);
    ChunkId chunkEnd(1 /*high*/, batchSize_ * (reqIndex + 1) /*low*/);
    ChainId chainId = chainIds_[reqIndex % chainIds_.size()];
    auto removeOp = storageClient_->createRemoveOp(chainId, chunkBegin, chunkEnd, batchSize_);
    removeOps.push_back(std::move(removeOp));
  }

  flat::UserInfo dummyUserInfo{};
  WriteOptions options;
  std::vector<folly::SemiFuture<folly::Expected<folly::Unit, hf3fs::Status>>> ioTasks;

  // start many coroutines to hit the concurrency limit and use all channel ids
  for (size_t reqIndex = 0; reqIndex < numConcurrentReqs_; reqIndex++) {
    auto ops = std::span<RemoveChunksOp>(&removeOps[reqIndex], 1);
    auto task = storageClient_->removeChunks(ops, dummyUserInfo, options).scheduleOn(&requestExe_).start();
    ioTasks.push_back(std::move(task));
  }

  folly::coro::blockingWait(folly::coro::collectAllRange(std::move(ioTasks)));

  for (const auto &removeOp : removeOps) {
    XLOGF(DBG5, "Remove range [{}, {})", removeOp.range.begin, removeOp.range.end);
    ASSERT_OK(removeOp.result.statusCode);
    ASSERT_LE(removeOp.result.numChunksRemoved, batchSize_);

    // check if chunks are removed
    auto queryOp =
        storageClient_->createQueryOp(removeOp.routingTarget.chainId, removeOp.range.begin, removeOp.range.end);
    folly::coro::blockingWait(storageClient_->queryLastChunk(std::span(&queryOp, 1), flat::UserInfo()));
    XLOGF(DBG5, "#chunks in range [{}, {}): {}", queryOp.range.begin, queryOp.range.end, queryOp.result.totalNumChunks);
    ASSERT_OK(queryOp.result.statusCode);
    ASSERT_EQ(0, queryOp.result.totalNumChunks);
    ASSERT_EQ(0, queryOp.result.totalChunkLen);
  }
}

TEST_P(TestStorageClientHCStress, ConcurrentReadRemove) {
  // register a block of memory
  std::vector<uint8_t> memoryBlock(setupConfig_.chunk_size() * numChunks_, 0xFF);
  auto regRes = storageClient_->registerIOBuffer(&memoryBlock[0], memoryBlock.size());
  ASSERT_OK(regRes);
  auto ioBuffer = std::move(*regRes);

  uint32_t maxLoops = chainIds_.size() * 2;

  for (uint32_t testLoop = 1; testLoop <= maxLoops; testLoop++) {
    // create chunks
    std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);
    ChainId chainId = chainIds_[testLoop % chainIds_.size()];
    auto result = writeToChunks(chainId, ChunkId(1, 0), ChunkId(1, numChunks_), chunkData);
    ASSERT_TRUE(result);

    // create remove ops

    std::vector<RemoveChunksOp> removeOps;

    for (size_t reqIndex = 0; reqIndex < numConcurrentReqs_; reqIndex++) {
      ChunkId chunkBegin(1 /*high*/, batchSize_ * reqIndex /*low*/);
      ChunkId chunkEnd(1 /*high*/, batchSize_ * (reqIndex + 1) /*low*/);
      auto removeOp = storageClient_->createRemoveOp(chainId, chunkBegin, chunkEnd, batchSize_);
      removeOps.push_back(std::move(removeOp));
    }

    // create read IOs

    std::vector<ReadIO> readIOs;
    size_t chunkIndex = 0;

    for (size_t reqIndex = 0; reqIndex < numConcurrentReqs_; reqIndex++) {
      for (size_t ioIndex = 0; ioIndex < batchSize_; ioIndex++, chunkIndex++) {
        ChunkId chunkId(1 /*high*/, chunkIndex /*low*/);
        auto readIO = storageClient_->createReadIO(chainId,
                                                   chunkId,
                                                   0 /*offset*/,
                                                   setupConfig_.chunk_size() /*length*/,
                                                   &memoryBlock[chunkIndex * setupConfig_.chunk_size()],
                                                   &ioBuffer);
        readIOs.push_back(std::move(readIO));
      }
    }

    flat::UserInfo dummyUserInfo{};

    // start read tasks
    std::vector<folly::SemiFuture<folly::Expected<folly::Unit, hf3fs::Status>>> readTasks;
    ReadOptions readOptions;

    for (size_t reqIndex = 0; reqIndex < numConcurrentReqs_; reqIndex++) {
      auto ios = std::span<ReadIO>(&readIOs[batchSize_ * reqIndex], &readIOs[batchSize_ * (reqIndex + 1)]);
      auto task = storageClient_->batchRead(ios, dummyUserInfo, readOptions).scheduleOn(&requestExe_).start();
      readTasks.push_back(std::move(task));
    }

    // start remove tasks
    std::vector<folly::SemiFuture<folly::Expected<folly::Unit, hf3fs::Status>>> removeTasks;
    WriteOptions removeOptions;

    for (size_t reqIndex = 0; reqIndex < numConcurrentReqs_; reqIndex++) {
      auto ops = std::span<RemoveChunksOp>(&removeOps[reqIndex], 1);
      auto task = storageClient_->removeChunks(ops, dummyUserInfo, removeOptions).scheduleOn(&requestExe_).start();
      removeTasks.push_back(std::move(task));
    }

    folly::coro::blockingWait(folly::coro::collectAllRange(std::move(readTasks)));
    folly::coro::blockingWait(folly::coro::collectAllRange(std::move(removeTasks)));

    for (const auto &removeOp : removeOps) {
      XLOGF(DBG5, "Remove chunks in {}", removeOp.range);
      ASSERT_OK(removeOp.result.statusCode);
      ASSERT_LE(removeOp.result.numChunksRemoved, batchSize_);
    }
  }
}

ClientTestConfig standardTimeout = {
    3_s /*initWaitTime*/,
    10_s /*maxWaitTime*/,
    300_s /*maxRetryTime*/,
    16U /*batchSize*/,
    8U /*maxBatchSize*/,
    32U /*numConcurrentReqs*/,
    16U /*maxConcurrentReqs*/,
};

ClientTestConfig onecoroBusyRetry = {
    1_ms /*initWaitTime*/,
    10_s /*maxWaitTime*/,
    300_s /*maxRetryTime*/,
    16U /*batchSize*/,
    16U /*maxBatchSize*/,
    1U /*numConcurrentReqs*/,
    1U /*maxConcurrentReqs*/,
};

ClientTestConfig multicoroBusyRetry = {
    1_ms /*initWaitTime*/,
    10_s /*maxWaitTime*/,
    300_s /*maxRetryTime*/,
    16U /*batchSize*/,
    8U /*maxBatchSize*/,
    32U /*numConcurrentReqs*/,
    16U /*maxConcurrentReqs*/,
};

INSTANTIATE_TEST_SUITE_P(StandardTimeout, TestStorageClientHCStress, ::testing::Values(standardTimeout));

INSTANTIATE_TEST_SUITE_P(OneCoroBusyRetry, TestStorageClientHCStress, ::testing::Values(onecoroBusyRetry));

INSTANTIATE_TEST_SUITE_P(MultiCoroBusyRetry, TestStorageClientHCStress, ::testing::Values(multicoroBusyRetry));

}  // namespace
}  // namespace hf3fs::storage::client
