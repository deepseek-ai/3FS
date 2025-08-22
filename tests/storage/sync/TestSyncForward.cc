#include <thread>

#include "client/storage/TargetSelection.h"
#include "common/serde/Serde.h"
#include "kv/KVStore.h"
#include "kv/MemDBStore.h"
#include "storage/service/StorageServer.h"
#include "tests/GtestHelpers.h"
#include "tests/lib/Helper.h"
#include "tests/lib/UnitTestFabric.h"

namespace hf3fs::storage::test {
namespace {

using namespace hf3fs::test;

class TestSyncForward : public UnitTestFabric, public ::testing::Test {
 protected:
  TestSyncForward()
      : UnitTestFabric(SystemSetupConfig{
            128_KB /*chunkSize*/,
            1 /*numChains*/,
            3 /*numReplicas*/,
            3 /*numStorageNodes*/,
            {folly::fs::temp_directory_path()} /*dataPaths*/,
            hf3fs::Path() /*clientConfig*/,
            hf3fs::Path() /*serverConfig*/,
            {} /*storageEndpoints*/,
            0 /*serviceLevel*/,
            0 /*listenPort*/,
            client::StorageClient::ImplementationType::RPC /*clientImplType*/,
            kv::KVStore::Type::RocksDB /*metaStoreType*/,
            true /*useFakeMgmtdClient*/,
        }) {}

  void SetUp() override {
    net::IBDevice::Config ibConfig;
    auto ibResult = net::IBManager::start(ibConfig);
    ASSERT_OK(ibResult);
    ASSERT_TRUE(setUpStorageSystem());
  }

  void TearDown() override { tearDownStorageSystem(); }
};

TEST_F(TestSyncForward, ForwardToSyncingTarget) {
  updateRoutingInfo([&](auto &routingInfo) {
    for (auto &[id, chain] : routingInfo.chains) {
      chain.targets.back().publicState = flat::PublicTargetState::OFFLINE;
      chain.chainVersion = flat::ChainVersion{1};
      routingInfo.targets[chain.targets.back().targetId].publicState = flat::PublicTargetState::OFFLINE;
    }
  });

  // 1. register a block of memory.
  std::vector<uint8_t> memoryBlock(setupConfig_.chunk_size(), 0xFF);
  auto regRes = storageClient_->registerIOBuffer(&memoryBlock[0], memoryBlock.size());
  ASSERT_OK(regRes);
  std::fill(memoryBlock.begin(), memoryBlock.end(), 0x01);

  // 2. create write io.
  auto ioBuffer = std::move(*regRes);
  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);
  auto writeIO = storageClient_->createWriteIO(chainId,
                                               chunkId,
                                               0 /*offset*/,
                                               setupConfig_.chunk_size() /*length*/,
                                               setupConfig_.chunk_size() /*chunkSize*/,
                                               &memoryBlock[0],
                                               &ioBuffer);

  // 3. first write.
  client::WriteOptions options;
  folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo(), options));
  ASSERT_OK(writeIO.result.lengthInfo);
  ASSERT_EQ(writeIO.length, writeIO.result.lengthInfo.value());

  // 4. update routing info.
  updateRoutingInfo([&](auto &routingInfo) {
    for (auto &[id, chain] : routingInfo.chains) {
      chain.targets.back().publicState = flat::PublicTargetState::SYNCING;
      chain.chainVersion = flat::ChainVersion{2};
      routingInfo.targets[chain.targets.back().targetId].publicState = flat::PublicTargetState::SYNCING;
    }
  });

  std::this_thread::sleep_for(2000_ms);  // wait sync start.
  RoutingStoreHelper::refreshRoutingInfo(*storageServers_.back());

  // 5. second write.
  {
    std::fill(memoryBlock.begin(), memoryBlock.end(), 0x02);
    auto writeIO = storageClient_->createWriteIO(chainId,
                                                 ChunkId(1, 2),
                                                 0 /*offset*/,
                                                 setupConfig_.chunk_size() /*length*/,
                                                 setupConfig_.chunk_size() /*chunkSize*/,
                                                 &memoryBlock[0],
                                                 &ioBuffer);
    folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo(), options));
    ASSERT_OK(writeIO.result.lengthInfo);
    ASSERT_EQ(writeIO.length, writeIO.result.lengthInfo.value());
  }

  for (auto waitTimes = 0_s; waitTimes < 10_s; waitTimes += 1_s) {
    std::this_thread::sleep_for(1_s);  // wait sync done.
    if (TargetMapHelper::checkLocalTargetState(*storageServers_.back(), flat::LocalTargetState::UPTODATE)) {
      break;
    }
  }
  ASSERT_TRUE(TargetMapHelper::checkLocalTargetState(*storageServers_.back(), flat::LocalTargetState::UPTODATE));

  // 6. update routing info.
  updateRoutingInfo([&](auto &routingInfo) {
    for (auto &[id, chain] : routingInfo.chains) {
      chain.targets.back().publicState = flat::PublicTargetState::SERVING;
      chain.chainVersion = flat::ChainVersion{3};
      routingInfo.targets[chain.targets.back().targetId].publicState = flat::PublicTargetState::SERVING;
    }
  });

  client::ReadOptions readOptions;
  readOptions.targetSelection().set_mode(client::TargetSelectionMode::TailTarget);
  std::vector<uint8_t> readBlock(setupConfig_.chunk_size(), 0xFF);
  auto readResult = readFromChunk(chainId, ChunkId(1, 1), readBlock, 0, 0, readOptions);
  ASSERT_OK(readResult.lengthInfo);
  ASSERT_EQ(*readResult.lengthInfo, readBlock.size());
  auto isAllOne = std::all_of(readBlock.begin(), readBlock.end(), [](uint8_t c) { return c == 1; });
  if (!isAllOne) {
    for (auto i = 0u; i < readBlock.size(); ++i) {
      if (readBlock[i] != 1) {
        XLOGF(ERR, "index {} value {}", i, readBlock[i]);
        ASSERT_EQ(readBlock[i], 1);
      }
    }
  }

  readResult = readFromChunk(chainId, ChunkId(1, 2), readBlock, 0, 0, readOptions);
  ASSERT_OK(readResult.lengthInfo);
  ASSERT_EQ(*readResult.lengthInfo, readBlock.size());
  auto isAllTwo = std::all_of(readBlock.begin(), readBlock.end(), [](uint8_t c) { return c == 2; });
  if (!isAllTwo) {
    for (auto i = 0u; i < readBlock.size(); ++i) {
      if (readBlock[i] != 2) {
        XLOGF(ERR, "index {} value {}", i, readBlock[i]);
        ASSERT_EQ(readBlock[i], 2);
        break;
      }
    }
  }

  // 7. third write.
  {
    std::fill(memoryBlock.begin(), memoryBlock.end(), 0x03);
    auto writeIO = storageClient_->createWriteIO(chainId,
                                                 ChunkId(1, 3),
                                                 0 /*offset*/,
                                                 setupConfig_.chunk_size() /*length*/,
                                                 setupConfig_.chunk_size() /*chunkSize*/,
                                                 &memoryBlock[0],
                                                 &ioBuffer);
    folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo(), options));
    ASSERT_OK(writeIO.result.lengthInfo);
    ASSERT_EQ(writeIO.length, writeIO.result.lengthInfo.value());
  }

  readResult = readFromChunk(chainId, ChunkId(1, 3), readBlock, 0, 0, readOptions);
  ASSERT_OK(readResult.lengthInfo);
  ASSERT_EQ(*readResult.lengthInfo, readBlock.size());
  ASSERT_TRUE(std::all_of(readBlock.begin(), readBlock.end(), [](uint8_t c) { return c == 3; }));
}

TEST_F(TestSyncForward, SyncingBatch) {
  constexpr auto kChunkNum = 16u;

  XLOGF(WARNING, "1. register a block of memory.");
  std::vector<uint8_t> memoryBlock(setupConfig_.chunk_size(), 0x01);
  auto regRes = storageClient_->registerIOBuffer(&memoryBlock[0], memoryBlock.size());
  ASSERT_OK(regRes);
  auto ioBuffer = std::move(*regRes);
  auto chainId = firstChainId_;

  XLOGF(WARNING, "2. write a batch of chunks.");
  for (auto i = 0u; i < kChunkNum; ++i) {
    std::fill(memoryBlock.begin(), memoryBlock.end(), 0x10 + i);
    ChunkId chunkId(1 /*high*/, i /*low*/);
    auto writeIO = storageClient_->createWriteIO(chainId,
                                                 chunkId,
                                                 0 /*offset*/,
                                                 setupConfig_.chunk_size() /*length*/,
                                                 setupConfig_.chunk_size() /*chunkSize*/,
                                                 &memoryBlock[0],
                                                 &ioBuffer);

    client::WriteOptions options;
    folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo(), options));
    ASSERT_OK(writeIO.result.lengthInfo);
    ASSERT_EQ(writeIO.length, writeIO.result.lengthInfo.value());
  }

  XLOGF(WARNING, "3. check chunk content.");
  client::ReadOptions readOptions;
  readOptions.targetSelection().set_mode(client::TargetSelectionMode::TailTarget);
  std::vector<uint8_t> readBlock(setupConfig_.chunk_size(), 0xFF);
  for (auto i = 0u; i < kChunkNum; ++i) {
    ChunkId chunkId(1 /*high*/, i /*low*/);
    auto readResult = readFromChunk(chainId, chunkId, readBlock, 0, 0, readOptions);
    ASSERT_OK(readResult.lengthInfo);
    ASSERT_EQ(*readResult.lengthInfo, readBlock.size());
    auto expect = 0x10 + i;
    for (auto j = 0u; j < readBlock.size(); ++j) {
      if (readBlock[j] != expect) {
        XLOGF(ERR, "i: {} j: {} value: {}, expect: {}", i, j, readBlock[j], expect);
        ASSERT_TRUE(false);
      }
    }
  }

  XLOGF(WARNING, "4. offline the last server.");
  updateRoutingInfo([&](auto &routingInfo) {
    for (auto &[id, chain] : routingInfo.chains) {
      chain.targets.back().publicState = flat::PublicTargetState::OFFLINE;
      chain.chainVersion = flat::ChainVersion{2};
      routingInfo.targets[chain.targets.back().targetId].publicState = flat::PublicTargetState::OFFLINE;
    }
  });

  XLOGF(WARNING, "5. second write [0, 1/4).");
  for (auto i = 0u; i < kChunkNum / 4; ++i) {
    std::fill(memoryBlock.begin(), memoryBlock.end(), 0x20 + i);
    ChunkId chunkId(1 /*high*/, i /*low*/);
    auto writeIO = storageClient_->createWriteIO(chainId,
                                                 chunkId,
                                                 0 /*offset*/,
                                                 setupConfig_.chunk_size() /*length*/,
                                                 setupConfig_.chunk_size() /*chunkSize*/,
                                                 &memoryBlock[0],
                                                 &ioBuffer);

    client::WriteOptions options;
    folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo(), options));
    ASSERT_OK(writeIO.result.lengthInfo);
    ASSERT_EQ(writeIO.length, writeIO.result.lengthInfo.value());
  }

  XLOGF(WARNING, "6. remove chunks [1/2, 1).");
  for (auto i = kChunkNum / 2; i < kChunkNum; ++i) {
    auto removeIO =
        storageClient_->createRemoveOp(chainId, ChunkId{1, i}, ChunkId{1, i + 1}, 1 /*maxNumChunkIdsToProcess*/);
    folly::coro::blockingWait(storageClient_->removeChunks(std::span(&removeIO, 1), flat::UserInfo()));
    ASSERT_OK(removeIO.result.statusCode);
    ASSERT_LE(removeIO.result.numChunksRemoved, 1);
  }

  XLOGF(WARNING, "7. check content.");
  readOptions.targetSelection().set_mode(client::TargetSelectionMode::Default);
  for (auto i = 0u; i < kChunkNum / 2; ++i) {
    ChunkId chunkId(1 /*high*/, i /*low*/);
    auto readResult = readFromChunk(chainId, chunkId, readBlock, 0, 0, readOptions);
    ASSERT_OK(readResult.lengthInfo);
    ASSERT_EQ(*readResult.lengthInfo, readBlock.size());
    auto expect = i < kChunkNum / 4 ? 0x20 + i : 0x10 + i;
    for (auto j = 0u; j < readBlock.size(); ++j) {
      if (readBlock[j] != expect) {
        XLOGF(ERR, "i: {} j: {} value: {}, expect: {}", i, j, readBlock[j], expect);
        ASSERT_TRUE(false);
      }
    }
  }
  for (auto i = kChunkNum / 2; i < kChunkNum; ++i) {
    ChunkId chunkId(1 /*high*/, i /*low*/);
    auto readResult = readFromChunk(chainId, chunkId, readBlock, 0, 0, readOptions);
    ASSERT_FALSE(readResult.lengthInfo);
    ASSERT_EQ(readResult.lengthInfo.error().code(), StorageClientCode::kChunkNotFound);
  }

  XLOGF(WARNING, "8. syncing the last server.");
  updateRoutingInfo([&](auto &routingInfo) {
    for (auto &[id, chain] : routingInfo.chains) {
      chain.targets.back().publicState = flat::PublicTargetState::SYNCING;
      chain.chainVersion = flat::ChainVersion{2};
      routingInfo.targets[chain.targets.back().targetId].publicState = flat::PublicTargetState::SYNCING;
    }
  });

  for (auto i = kChunkNum / 8; i < kChunkNum * 3 / 8; ++i) {
    std::fill(memoryBlock.begin(), memoryBlock.end(), 0x30 + i);
    ChunkId chunkId(1 /*high*/, i /*low*/);
    auto writeIO = storageClient_->createWriteIO(chainId,
                                                 chunkId,
                                                 0 /*offset*/,
                                                 setupConfig_.chunk_size() /*length*/,
                                                 setupConfig_.chunk_size() /*chunkSize*/,
                                                 &memoryBlock[0],
                                                 &ioBuffer);

    client::WriteOptions options;
    options.retry().set_init_wait_time(3000_ms);
    options.retry().set_max_wait_time(3000_ms);
    options.retry().set_max_retry_time(5000_ms);
    folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo(), options));
    ASSERT_OK(writeIO.result.lengthInfo);
    ASSERT_EQ(writeIO.length, writeIO.result.lengthInfo.value());
  }

  for (auto waitTimes = 0_s; waitTimes < 30_s; waitTimes += 1_s) {
    std::this_thread::sleep_for(1_s);  // wait sync done.
    if (TargetMapHelper::checkLocalTargetState(*storageServers_.back(), flat::LocalTargetState::UPTODATE)) {
      break;
    }
  }
  ASSERT_TRUE(TargetMapHelper::checkLocalTargetState(*storageServers_.back(), flat::LocalTargetState::UPTODATE));

  XLOGF(WARNING, "9. online the last server.");
  updateRoutingInfo([&](auto &routingInfo) {
    for (auto &[id, chain] : routingInfo.chains) {
      chain.targets.back().publicState = flat::PublicTargetState::SERVING;
      chain.chainVersion = flat::ChainVersion{3};
      routingInfo.targets[chain.targets.back().targetId].publicState = flat::PublicTargetState::SERVING;
    }
  });

  XLOGF(WARNING, "10. check content.");
  readOptions.targetSelection().set_mode(client::TargetSelectionMode::TailTarget);
  for (auto i = 0u; i < kChunkNum / 2; ++i) {
    ChunkId chunkId(1 /*high*/, i /*low*/);
    auto readResult = readFromChunk(chainId, chunkId, readBlock, 0, 0, readOptions);
    ASSERT_OK(readResult.lengthInfo);
    ASSERT_EQ(*readResult.lengthInfo, readBlock.size());
    auto expect = (i < kChunkNum / 8 ? 0x20 : i < kChunkNum * 3 / 8 ? 0x30 : 0x10) + i;
    for (auto j = 0u; j < readBlock.size(); ++j) {
      if (readBlock[j] != expect) {
        XLOGF(ERR, "i: {} j: {} value: {}, expect: {}", i, j, readBlock[j], expect);
        ASSERT_TRUE(false);
      }
    }
  }
  for (auto i = kChunkNum / 2; i < kChunkNum; ++i) {
    ChunkId chunkId(1 /*high*/, i /*low*/);
    auto readResult = readFromChunk(chainId, chunkId, readBlock, 0, 0, readOptions);
    ASSERT_FALSE(readResult.lengthInfo);
    ASSERT_EQ(readResult.lengthInfo.error().code(), StorageClientCode::kChunkNotFound);
  }
}

}  // namespace
}  // namespace hf3fs::storage::test
