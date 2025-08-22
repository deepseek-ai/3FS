#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/net/Client.h"
#include "tests/lib/Helper.h"
#include "tests/lib/UnitTestFabric.h"

namespace hf3fs::storage {
namespace {

using namespace hf3fs::test;

class TestIncorrectRoutingInfo : public UnitTestFabric, public ::testing::Test {
 protected:
  TestIncorrectRoutingInfo()
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
            client::StorageClient::ImplementationType::RPC,
            kv::KVStore::Type::RocksDB,
            true /*useFakeMgmtdClient*/,
        }) {}

  void SetUp() override {
    // init ib device
    net::IBDevice::Config ibConfig;
    auto ibResult = net::IBManager::start(ibConfig);
    ASSERT_OK(ibResult);
    ASSERT_TRUE(setUpStorageSystem());
    clientConfig_.retry().set_max_retry_time(10_s);
  }

  void TearDown() override { tearDownStorageSystem(); }
};

TEST_F(TestIncorrectRoutingInfo, MismatchChainVersion) {
  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);

  // create a test chunk
  auto ioResult = writeToChunk(chainId, chunkId, chunkData);
  ASSERT_RESULT_EQ(chunkData.size(), ioResult.lengthInfo);

  // get the first chain
  auto newRoutingInfo = copyRoutingInfo();
  auto &chainTable = *newRoutingInfo->getChainTable(kTableId());
  auto &firstChain = newRoutingInfo->chains[chainTable.chains.front()];
  ASSERT_EQ(chainId, firstChain.chainId);
  auto chainVersion = firstChain.chainVersion;

  for (int32_t deltaVer : {-1, 1}) {
    // change version of the first chain
    firstChain.chainVersion = flat::ChainVersion(chainVersion + deltaVer);
    // only update client's routing info
    auto fakeClient = dynamic_cast<FakeMgmtdClient *>(mgmtdForClient_.get());
    newRoutingInfo->routingInfoVersion++;
    fakeClient->setRoutingInfo(newRoutingInfo);
    ASSERT_OK(folly::coro::blockingWait(mgmtdForClient_->refreshRoutingInfo(/*force=*/false)));
    // try to read/write with mismatch chain version
    auto readRes = readFromChunk(chainId, chunkId, chunkData, 0, chunkData.size());
    ASSERT_ERROR(readRes.lengthInfo, StorageClientCode::kRoutingVersionMismatch);
    auto writeRes = writeToChunk(chainId, chunkId, chunkData, 0, chunkData.size());
    ASSERT_ERROR(writeRes.lengthInfo, StorageClientCode::kRoutingVersionMismatch);
  }
}

TEST_F(TestIncorrectRoutingInfo, WriteNotSentToHeadTarget) {
  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);

  // send write to the second target on chain
  client::WriteOptions options;
  options.targetSelection().set_mode(client::TargetSelectionMode::ManualMode);
  options.targetSelection().set_targetIndex(1);
  auto ioResult = writeToChunk(chainId, chunkId, chunkData, 0, chunkData.size(), options);
  ASSERT_ERROR(ioResult.lengthInfo, StorageClientCode::kRoutingError);
}

TEST_F(TestIncorrectRoutingInfo, RemoveNotSentToHeadTarget) {
  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);

  // create a test chunk
  auto ioResult = writeToChunk(chainId, chunkId, chunkData);
  ASSERT_RESULT_EQ(chunkData.size(), ioResult.lengthInfo);

  // send remove to the second target on chain
  client::WriteOptions options;
  options.targetSelection().set_mode(client::TargetSelectionMode::ManualMode);
  options.targetSelection().set_targetIndex(1);
  auto removeOp = storageClient_->createRemoveOp(chainId,
                                                 ChunkId(chunkId, 0),
                                                 ChunkId(chunkId, 0xFF),
                                                 1 /*maxNumChunkIdsToProcess*/);
  folly::coro::blockingWait(storageClient_->removeChunks(std::span(&removeOp, 1), flat::UserInfo(), options));
  ASSERT_ERROR(removeOp.result.statusCode, StorageClientCode::kRoutingError);
}

TEST_F(TestIncorrectRoutingInfo, WorkingHeadDetectedAsOffline) {
  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);

  auto regRes = storageClient_->registerIOBuffer(&chunkData[0], chunkData.size());
  ASSERT_OK(regRes);
  auto ioBuffer = std::move(*regRes);

  size_t numWriteIOs = 100;
  std::vector<client::WriteIO> writeIOs;

  // create write IOs to one chunk
  for (size_t writeIndex = 0; writeIndex < numWriteIOs; writeIndex++) {
    auto writeIO = storageClient_->createWriteIO(chainId,
                                                 chunkId,
                                                 0 /*offset*/,
                                                 chunkData.size() /*length*/,
                                                 setupConfig_.chunk_size() /*chunkSize*/,
                                                 &chunkData[0],
                                                 &ioBuffer);
    writeIOs.push_back(std::move(writeIO));
  }

  // issue the write requests
  flat::UserInfo dummyUserInfo{};
  auto options = client::WriteOptions();

  std::vector<folly::SemiFuture<folly::Expected<folly::Unit, hf3fs::Status>>> writeTasks;

  for (auto &writeIO : writeIOs) {
    auto writeTask = storageClient_->write(writeIO, dummyUserInfo, options).scheduleOn(&requestExe_).start();
    writeTasks.push_back(std::move(writeTask));
  }

  // set head target to offline state but it's actually working
  auto newRoutingInfo = copyRoutingInfo();
  setTargetOffline(*newRoutingInfo, 0);

  // let all storage servers except the host of head targets know the latest routing info
  for (size_t serverIndex = 1; serverIndex < storageServers_.size(); serverIndex++) {
    auto &storageServer = storageServers_[serverIndex];
    auto client = RoutingStoreHelper::getMgmtdClient(*storageServer);
    auto fakeClient = dynamic_cast<FakeMgmtdClient *>(client.get());
    fakeClient->setRoutingInfo(newRoutingInfo);
    RoutingStoreHelper::refreshRoutingInfo(*storageServer);
  }

  // client does not konw the head is marked offline yet, so let it retry for a short time
  std::this_thread::sleep_for(500_ms);

#if defined(__has_feature)
#if !__has_feature(thread_sanitizer)
  for (auto &writeIO : writeIOs) {
    // some writes are completed but others are still under retry
    if (writeIO.result.lengthInfo) {
      ASSERT_RESULT_EQ(writeIO.length, writeIO.result.lengthInfo);
    } else {
      switch (writeIO.statusCode()) {
        case StorageClientCode::kCommError:
        case StorageClientCode::kTimeout:
        case StorageClientCode::kRoutingVersionMismatch:
          XLOGF(INFO, "Write IO {} length info: {}", fmt::ptr(&writeIO), writeIO.result.lengthInfo);
          break;
        default:
          ASSERT_EQ(StorageClientCode::kNotInitialized, writeIO.result.lengthInfo.error().code())
              << fmt::format("Write IO {} length info: {}", fmt::ptr(&writeIO), writeIO.result.lengthInfo);
      }
    }
  }
#endif
#endif

  // let client know the latest routing info
  auto fakeClient = dynamic_cast<FakeMgmtdClient *>(mgmtdForClient_.get());
  fakeClient->setRoutingInfo(newRoutingInfo);
  ASSERT_OK(folly::coro::blockingWait(mgmtdForClient_->refreshRoutingInfo(/*force=*/false)));

  // wait until all write tasks completed
  folly::coro::blockingWait(folly::coro::collectAllRange(std::move(writeTasks)));

  // check all write IOs have succeeded
  std::set<ChunkVer> updateVersions;
  for (const auto &writeIO : writeIOs) {
    ASSERT_OK(writeIO.result.lengthInfo);
    // check commit version == update version
    ASSERT_EQ(writeIO.result.commitVer, writeIO.result.updateVer);
    // check the update versions are in range [1..numWriteIOs]
    ASSERT_LE(1, writeIO.result.updateVer);
    ASSERT_LE(writeIO.result.updateVer, numWriteIOs);
    updateVersions.insert(writeIO.result.updateVer);
  }
  // check the update versions are unique
  ASSERT_EQ(numWriteIOs, updateVersions.size());
}

}  // namespace
}  // namespace hf3fs::storage
