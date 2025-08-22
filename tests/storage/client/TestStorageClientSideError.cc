#include <folly/experimental/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/net/Client.h"
#include "tests/lib/UnitTestFabric.h"

namespace hf3fs::storage::client {
namespace {

using namespace hf3fs::test;

class TestStorageClientSideError : public UnitTestFabric, public ::testing::Test {
 protected:
  TestStorageClientSideError()
      : UnitTestFabric(SystemSetupConfig{128_KB /*chunkSize*/,
                                         1 /*numChains*/,
                                         1 /*numReplicas*/,
                                         1 /*numStorageNodes*/,
                                         {folly::fs::temp_directory_path()}}) {}

  void SetUp() override {
    // init ib device
    net::IBDevice::Config ibConfig;
    auto ibResult = net::IBManager::start(ibConfig);
    ASSERT_OK(ibResult);
    ASSERT_TRUE(setUpStorageSystem());
    clientConfig_.retry().set_max_retry_time(2_s);
  }

  void TearDown() override { tearDownStorageSystem(); }

  template <typename Op>
  void checkFailedOpsPtrs(const std::vector<Op> &ops, const std::vector<Op *> &failedOps) {
    bool ok = std::all_of(failedOps.cbegin(), failedOps.cend(), [&ops](const Op *failedOp) -> bool {
      for (const auto &op : ops) {
        if (failedOp == &op) return true;
      }
      XLOGF(ERR,
            "Address of failed op {} not in range: [{}, {})",
            fmt::ptr(failedOp),
            fmt::ptr(&ops[0]),
            fmt::ptr(&ops[ops.size() - 1]));
      return false;
    });
    ASSERT_TRUE(ok);
  }
};

TEST_F(TestStorageClientSideError, GetReplicationChainError) {
  updateRoutingInfo([&](auto &routingInfo) {
    auto &chainTable = *routingInfo.getChainTable(kTableId());
    auto &chainInfo = routingInfo.chains[chainTable.chains.front()];

    // first target offline
    chainInfo.targets.begin()->publicState = hf3fs::flat::PublicTargetState::OFFLINE;
  });

  std::vector<uint8_t> userData(1_KB, 0xFF);
  auto ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kNotAvailable);

  // inconsistent chain id
  updateRoutingInfo([&](auto &routingInfo) {
    auto &chainTable = *routingInfo.getChainTable(kTableId());
    auto &chainInfo = routingInfo.chains[chainTable.chains.front()];
    chainInfo.chainId = flat::ChainId(0);
  });

  ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kRoutingError);

  // empty chain table
  updateRoutingInfo([&](auto &routingInfo) {
    auto &chainTable = *routingInfo.getChainTable(kTableId());
    chainTable.chains.clear();
  });

  ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kRoutingError);
}

TEST_F(TestStorageClientSideError, GetStorageTargetError) {
  updateRoutingInfo([&](auto &routingInfo) {
    auto &targetInfo = routingInfo.targets.begin()->second;

    // host node id not unknown
    targetInfo.nodeId = std::nullopt;
  });

  std::vector<uint8_t> userData(1_KB, 0xFF);
  auto ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kNotAvailable);

  // target offline
  updateRoutingInfo([&](auto &routingInfo) {
    auto &targetInfo = routingInfo.targets.begin()->second;
    targetInfo.publicState = hf3fs::flat::PublicTargetState::OFFLINE;
  });

  ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kNotAvailable);

  // inconsistent target id
  updateRoutingInfo([&](auto &routingInfo) {
    auto &targetInfo = routingInfo.targets.begin()->second;
    targetInfo.targetId = flat::TargetId(0);
  });

  ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kRoutingError);

  // empty target list
  updateRoutingInfo([&](auto &routingInfo) { routingInfo.targets.clear(); });

  ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kRoutingError);
}

TEST_F(TestStorageClientSideError, GetStorageNodeError) {
  updateRoutingInfo([&](auto &routingInfo) {
    auto &nodeInfo = routingInfo.nodes.begin()->second;

    // not storage node
    nodeInfo.type = hf3fs::flat::NodeType::META;
  });

  std::vector<uint8_t> userData(1_KB, 0xFF);
  auto ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kRoutingError);

  // inconsistent node id
  updateRoutingInfo([&](auto &routingInfo) {
    auto &nodeInfo = routingInfo.nodes.begin()->second;
    nodeInfo.app.nodeId = hf3fs::flat::NodeId(0);
  });

  ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kRoutingError);

  // empty node list
  updateRoutingInfo([&](auto &routingInfo) { routingInfo.nodes.clear(); });

  ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kRoutingError);
}

TEST_F(TestStorageClientSideError, WrongServerAddress) {
  updateRoutingInfo([&](auto &routingInfo) {
    auto &nodeInfo = routingInfo.nodes.begin()->second;

    // wrong port
    nodeInfo.app.serviceGroups.front().endpoints.front().port = 0;
  });

  std::vector<uint8_t> userData(1_KB, 0xFF);
  auto ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_TRUE(!ioResult.lengthInfo);
  ASSERT_TRUE(ioResult.lengthInfo.error().code() == StorageClientCode::kTimeout ||
              ioResult.lengthInfo.error().code() == StorageClientCode::kCommError);

  ioResult = readFromChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_TRUE(!ioResult.lengthInfo);
  ASSERT_TRUE(ioResult.lengthInfo.error().code() == StorageClientCode::kTimeout ||
              ioResult.lengthInfo.error().code() == StorageClientCode::kCommError);

  // empty address list
  updateRoutingInfo([&](auto &routingInfo) {
    auto &nodeInfo = routingInfo.nodes.begin()->second;
    nodeInfo.app.serviceGroups.front().endpoints.clear();
  });

  ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kNoRDMAInterface);

  ioResult = readFromChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kNoRDMAInterface);
}

TEST_F(TestStorageClientSideError, DifferentTrafficZone) {
  updateRoutingInfo([&](auto &routingInfo) {
    auto &nodeInfo = routingInfo.nodes.begin()->second;
    // set traffic zone of the host
    nodeInfo.tags = {flat::TagPair{flat::kTrafficZoneTagKey, "TEST_ZONE0"}};
  });

  {
    std::vector<uint8_t> userData(1_KB, 0xFF);
    auto ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
    ASSERT_RESULT_EQ(userData.size(), ioResult.lengthInfo);

    ReadOptions options;
    // client is in the same traffic zone as the storage node
    options.targetSelection().set_trafficZone("TEST_ZONE0");
    ioResult = readFromChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
    ASSERT_RESULT_EQ(userData.size(), ioResult.lengthInfo);
  }

  {
    std::vector<uint8_t> userData(1_KB, 0xFF);
    auto ioResult = writeToChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size());
    ASSERT_RESULT_EQ(userData.size(), ioResult.lengthInfo);

    ReadOptions options;
    // client is in a different traffic zone from the storage node
    options.targetSelection().set_trafficZone("TEST_ZONE1");
    ioResult = readFromChunk(firstChainId_, ChunkId(1, 1), userData, 0 /*offset*/, userData.size(), options);
    ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kNotAvailable);
  }
}

TEST_F(TestStorageClientSideError, InvalidDataRange) {
  std::vector<uint8_t> userData(1_KB, 0xFF);

  {
    // overlapping data buffers
    std::vector<uint8_t> memoryBlock(setupConfig_.chunk_size() * 2, 0xFF);
    auto regRes = storageClient_->registerIOBuffer(&memoryBlock[0], memoryBlock.size());
    ASSERT_TRUE(regRes);
    auto ioBuffer = std::move(*regRes);

    std::vector<ReadIO> readIOs;

    for (size_t offset = 0; offset < setupConfig_.chunk_size(); offset += setupConfig_.chunk_size() / 4) {
      auto readIO = storageClient_->createReadIO(firstChainId_,
                                                 ChunkId(1, 1),
                                                 offset /*offset*/,
                                                 setupConfig_.chunk_size() /*length*/,
                                                 &memoryBlock[offset],
                                                 &ioBuffer);
      readIOs.push_back(std::move(readIO));
    }

    std::vector<ReadIO *> failedIOs;
    folly::coro::blockingWait(storageClient_->batchRead(readIOs, flat::UserInfo(), ReadOptions(), &failedIOs));

    for (const auto &readIO : readIOs) {
      ASSERT_FALSE(readIO.result.lengthInfo);
      ASSERT_EQ(StorageClientCode::kInvalidArg, readIO.result.lengthInfo.error().code());
    }

    checkFailedOpsPtrs(readIOs, failedIOs);
    std::set<ReadIO *> uniqueFailedIOs(failedIOs.begin(), failedIOs.end());
    ASSERT_EQ(readIOs.size(), uniqueFailedIOs.size());
  }

  {
    // duplicate IOs
    std::vector<uint8_t> memoryBlock(setupConfig_.chunk_size(), 0xFF);
    auto regRes = storageClient_->registerIOBuffer(&memoryBlock[0], memoryBlock.size());
    ASSERT_TRUE(regRes);
    auto ioBuffer = std::move(*regRes);

    std::vector<ReadIO> readIOs;

    for (int i = 0; i < 2; i++) {
      auto readIO = storageClient_->createReadIO(firstChainId_,
                                                 ChunkId(1, 1),
                                                 0 /*offset*/,
                                                 setupConfig_.chunk_size() /*length*/,
                                                 &memoryBlock[0],
                                                 &ioBuffer);
      readIOs.push_back(std::move(readIO));
    }

    std::vector<ReadIO *> failedIOs;
    folly::coro::blockingWait(storageClient_->batchRead(readIOs, flat::UserInfo(), ReadOptions(), &failedIOs));

    for (const auto &readIO : readIOs) {
      ASSERT_FALSE(readIO.result.lengthInfo);
      ASSERT_EQ(StorageClientCode::kInvalidArg, readIO.result.lengthInfo.error().code());
    }

    checkFailedOpsPtrs(readIOs, failedIOs);
    std::set<ReadIO *> uniqueFailedIOs(failedIOs.begin(), failedIOs.end());
    ASSERT_EQ(readIOs.size(), uniqueFailedIOs.size());
  }

  {
    // null io buffer pointer
    auto writeIO = storageClient_->createWriteIO(firstChainId_,
                                                 ChunkId(1, 1),
                                                 0 /*offset*/,
                                                 userData.size() /*length*/,
                                                 setupConfig_.chunk_size(),
                                                 &userData[0],
                                                 nullptr);
    folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo()));
    ASSERT_FALSE(writeIO.result.lengthInfo);
    ASSERT_EQ(StorageClientCode::kInvalidArg, writeIO.result.lengthInfo.error().code());
  }

  {
    // null data pointer
    auto regRes = storageClient_->registerIOBuffer(&userData[0], userData.size());
    ASSERT_TRUE(regRes);
    auto ioBuffer = std::move(*regRes);

    auto writeIO = storageClient_->createWriteIO(firstChainId_,
                                                 ChunkId(1, 1),
                                                 0 /*offset*/,
                                                 userData.size() /*length*/,
                                                 setupConfig_.chunk_size(),
                                                 nullptr,
                                                 &ioBuffer);
    folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo()));
    ASSERT_FALSE(writeIO.result.lengthInfo);
    ASSERT_EQ(StorageClientCode::kInvalidArg, writeIO.result.lengthInfo.error().code());
  }

  {
    // write range is out of the chunk boundary
    std::vector<uint8_t> largeArray(setupConfig_.chunk_size() * 2, 0xFF);
    auto regRes = storageClient_->registerIOBuffer(&largeArray[0], largeArray.size());
    ASSERT_TRUE(regRes);
    auto ioBuffer = std::move(*regRes);

    auto writeIO = storageClient_->createWriteIO(firstChainId_,
                                                 ChunkId(1, 1),
                                                 0 /*offset*/,
                                                 largeArray.size() /*length*/,
                                                 setupConfig_.chunk_size(),
                                                 &largeArray[0],
                                                 &ioBuffer);
    folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo()));
    ASSERT_FALSE(writeIO.result.lengthInfo);
    ASSERT_EQ(StorageClientCode::kInvalidArg, writeIO.result.lengthInfo.error().code());
  }

  {
    // write range is out of the io buffer
    auto regRes = storageClient_->registerIOBuffer(&userData[0], userData.size());
    ASSERT_TRUE(regRes);
    auto ioBuffer = std::move(*regRes);

    auto writeIO = storageClient_->createWriteIO(firstChainId_,
                                                 ChunkId(1, 1),
                                                 0 /*offset*/,
                                                 2 * userData.size() /*length*/,
                                                 setupConfig_.chunk_size(),
                                                 &userData[0],
                                                 &ioBuffer);
    folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo()));
    ASSERT_FALSE(writeIO.result.lengthInfo);
    ASSERT_EQ(StorageClientCode::kInvalidArg, writeIO.result.lengthInfo.error().code());
  }
}

TEST_F(TestStorageClientSideError, InvalidChunkIdRange) {
  ChunkId largeChunkId(1 /*high*/, 3 /*low*/);
  ChunkId smallChunkId(1 /*high*/, 1 /*low*/);
  ASSERT_GT(largeChunkId, smallChunkId);

  {
    // invalid begin/end chunk ids for a range query
    auto queryOp =
        storageClient_->createQueryOp(firstChainId_, largeChunkId, smallChunkId, 1 /*maxNumChunkIdsToProcess*/);
    folly::coro::blockingWait(storageClient_->queryLastChunk(std::span(&queryOp, 1), flat::UserInfo()));
    ASSERT_FALSE(queryOp.result.statusCode);
    ASSERT_EQ(StorageClientCode::kInvalidArg, queryOp.result.statusCode.error().code());
  }
  {
    // invalid maxNumChunkIdsToProcess for a range query
    auto queryOp =
        storageClient_->createQueryOp(firstChainId_, smallChunkId, largeChunkId, 0 /*maxNumChunkIdsToProcess*/);
    folly::coro::blockingWait(storageClient_->queryLastChunk(std::span(&queryOp, 1), flat::UserInfo()));
    ASSERT_FALSE(queryOp.result.statusCode);
    ASSERT_EQ(StorageClientCode::kInvalidArg, queryOp.result.statusCode.error().code());
  }

  {
    // invalid begin/end chunk ids for a batch remove
    auto removeOp =
        storageClient_->createRemoveOp(firstChainId_, largeChunkId, smallChunkId, 1 /*maxNumChunkIdsToProcess*/);
    folly::coro::blockingWait(storageClient_->removeChunks(std::span(&removeOp, 1), flat::UserInfo()));
    ASSERT_FALSE(removeOp.result.statusCode);
    ASSERT_EQ(StorageClientCode::kInvalidArg, removeOp.result.statusCode.error().code());
  }
  {
    // invalid maxNumChunkIdsToProcess for a batch remove
    auto removeOp =
        storageClient_->createRemoveOp(firstChainId_, smallChunkId, largeChunkId, 0 /*maxNumChunkIdsToProcess*/);
    folly::coro::blockingWait(storageClient_->removeChunks(std::span(&removeOp, 1), flat::UserInfo()));
    ASSERT_FALSE(removeOp.result.statusCode);
    ASSERT_EQ(StorageClientCode::kInvalidArg, removeOp.result.statusCode.error().code());
  }
}

TEST_F(TestStorageClientSideError, InvalidChunkSize) {
  auto truncateOp = storageClient_->createTruncateOp(firstChainId_,
                                                     ChunkId(1 /*high*/, 1 /*low*/),
                                                     setupConfig_.chunk_size() * 2 /*chunkLen*/,
                                                     setupConfig_.chunk_size());
  folly::coro::blockingWait(storageClient_->truncateChunks(std::span(&truncateOp, 1), flat::UserInfo()));
  ASSERT_FALSE(truncateOp.result.lengthInfo);
  ASSERT_EQ(StorageClientCode::kInvalidArg, truncateOp.result.lengthInfo.error().code());
}

}  // namespace
}  // namespace hf3fs::storage::client
