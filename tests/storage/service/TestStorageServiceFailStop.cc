#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/net/Client.h"
#include "tests/lib/UnitTestFabric.h"

namespace hf3fs::storage {
namespace {

using namespace hf3fs::test;

using SystemFailureConfig =
    std::tuple<uint32_t /*numReplicas*/, uint32_t /*failedTargetIndex*/, bool /*useFakeMgmtdClient*/>;

std::string prettyPrintConfig(const testing::TestParamInfo<SystemFailureConfig> &info) {
  return fmt::format("{}of{}failed_fakemgmtd{}",
                     std::get<1>(info.param) + 1,
                     std::get<0>(info.param),
                     std::get<2>(info.param));
}

class TestStorageServiceFailStop : public UnitTestFabric, public ::testing::TestWithParam<SystemFailureConfig> {
 protected:
  TestStorageServiceFailStop()
      : UnitTestFabric(SystemSetupConfig{
            128_KB /*chunkSize*/,
            1 /*numChains*/,
            std::get<0>(GetParam()) /*numReplicas*/,
            std::get<0>(GetParam()) /*numStorageNodes*/,
            {folly::fs::temp_directory_path()} /*dataPaths*/,
            hf3fs::Path() /*clientConfig*/,
            hf3fs::Path() /*serverConfig*/,
            {} /*storageEndpoints*/,
            0 /*serviceLevel*/,
            0 /*listenPort*/,
            client::StorageClient::ImplementationType::RPC,
            kv::KVStore::Type::RocksDB,
            std::get<2>(GetParam()) /*useFakeMgmtdClient*/,
        }),
        failedTargetIndex_(std::get<1>(GetParam())) {}

  void SetUp() override {
    // init ib device
    net::IBDevice::Config ibConfig;
    auto ibResult = net::IBManager::start(ibConfig);
    ASSERT_OK(ibResult);
    ASSERT_TRUE(setUpStorageSystem());
  }

  void TearDown() override { tearDownStorageSystem(); }

 protected:
  uint32_t failedTargetIndex_;
};

TEST_P(TestStorageServiceFailStop, FailureUndetected) {
  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);

  // stop the target
  ASSERT_TRUE(stopAndRemoveStorageServer(failedTargetIndex_));

  // write fails after the target stops undetectedly
  clientConfig_.retry().set_max_retry_time(10_s);
  auto ioResult = writeToChunk(chainId, chunkId, chunkData);
  ASSERT_TRUE(ioResult.lengthInfo.hasError());
  if (failedTargetIndex_ == 0) {
    switch (ioResult.lengthInfo.error().code()) {
      case StorageClientCode::kCommError:
      case StorageClientCode::kTimeout:
        break;
      default:
        ASSERT_TRUE(ioResult.lengthInfo.error().code());
    }
  } else
    ASSERT_EQ(StorageClientCode::kResourceBusy, ioResult.lengthInfo.error().code());
}

TEST_P(TestStorageServiceFailStop, FailureDetectedBeforeWrite) {
  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);

  // stop the target
  ASSERT_TRUE(stopAndRemoveStorageServer(failedTargetIndex_));

  // set the target to offline state
  ASSERT_TRUE(updateRoutingInfo([&](auto &routingInfo) { setTargetOffline(routingInfo, failedTargetIndex_); }));

  // write succeeds after the failure is detected
  if (storageServers_.empty()) clientConfig_.retry().set_max_retry_time(10_s);
  auto ioResult = writeToChunk(chainId, chunkId, chunkData, 0 /*offset*/, chunkData.size());

  if (storageServers_.empty()) {
    // only single replica
    ASSERT_TRUE(ioResult.lengthInfo.hasError());
    ASSERT_EQ(StorageClientCode::kNotAvailable, ioResult.lengthInfo.error().code());
  } else {
    // multiple replicas
    ASSERT_OK(ioResult.lengthInfo);
    ASSERT_EQ(chunkData.size(), ioResult.lengthInfo.value());
  }
}

TEST_P(TestStorageServiceFailStop, FailureDetectedDuringRetry) {
  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);

  auto regRes = storageClient_->registerIOBuffer(&chunkData[0], chunkData.size());
  ASSERT_OK(regRes);

  // create write IO
  auto ioBuffer = std::move(*regRes);
  auto writeIO = storageClient_->createWriteIO(chainId,
                                               chunkId,
                                               0 /*offset*/,
                                               chunkData.size() /*length*/,
                                               setupConfig_.chunk_size() /*chunkSize*/,
                                               &chunkData[0],
                                               &ioBuffer);

  // stop the target
  ASSERT_TRUE(stopAndRemoveStorageServer(failedTargetIndex_));

  // issue the write request
  flat::UserInfo dummyUserInfo{};
  auto options = client::WriteOptions();
  options.retry().set_max_retry_time(5_s);
  auto writeTask = storageClient_->write(writeIO, dummyUserInfo, options).scheduleOn(&requestExe_).start();

  // retry for a short time
  std::this_thread::sleep_for(2000_ms);
  // check the request failed with communication error
#if defined(__has_feature)
#if !__has_feature(thread_sanitizer)
  ASSERT_FALSE(writeIO.result.lengthInfo);
  if (failedTargetIndex_ == 0) {
    switch (writeIO.result.lengthInfo.error().code()) {
      case StorageClientCode::kCommError:
      case StorageClientCode::kTimeout:
        break;
      default:
        ASSERT_TRUE(writeIO.result.lengthInfo.error().code());
    }
  } else
    ASSERT_EQ(StorageClientCode::kResourceBusy, writeIO.result.lengthInfo.error().code());
#endif
#endif
  // set the target to offline state
  ASSERT_TRUE(updateRoutingInfo([&](auto &routingInfo) { setTargetOffline(routingInfo, failedTargetIndex_); }));

  writeTask.wait();

  if (storageServers_.empty()) {
    // only single replica
    ASSERT_TRUE(writeIO.result.lengthInfo.hasError());
    ASSERT_EQ(StorageClientCode::kNotAvailable, writeIO.result.lengthInfo.error().code());
  } else {
    // multiple replicas
    ASSERT_OK(writeIO.result.lengthInfo);
    ASSERT_EQ(chunkData.size(), writeIO.result.lengthInfo.value());

    // read and check chunk data
    std::vector<uint8_t> readData(chunkData.size());
    auto readRes = readFromChunk(chainId, chunkId, readData);
    ASSERT_OK(readRes.lengthInfo);
    ASSERT_EQ(1, readRes.commitVer);
    ASSERT_EQ(1, readRes.updateVer);
    ASSERT_EQ(chunkData.size(), *readRes.lengthInfo);
    ASSERT_EQ(chunkData, readData);
  }
}

TEST_P(TestStorageServiceFailStop, ConcurrentWrites) {
  auto chainId = firstChainId_;
  ChunkId chunkId(1 /*high*/, 1 /*low*/);
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);

  auto regRes = storageClient_->registerIOBuffer(&chunkData[0], chunkData.size());
  ASSERT_OK(regRes);
  auto ioBuffer = std::move(*regRes);

  size_t numWriteIOs = 10;
  std::vector<client::WriteIO> writeIOs;

  for (size_t writeIndex = 0; writeIndex < numWriteIOs; writeIndex++) {
    // create write IO
    auto writeIO = storageClient_->createWriteIO(chainId,
                                                 chunkId,
                                                 0 /*offset*/,
                                                 chunkData.size() /*length*/,
                                                 setupConfig_.chunk_size() /*chunkSize*/,
                                                 &chunkData[0],
                                                 &ioBuffer);
    writeIOs.push_back(std::move(writeIO));
  }

  // stop the target
  ASSERT_TRUE(stopAndRemoveStorageServer(failedTargetIndex_));

  // issue the write request
  flat::UserInfo dummyUserInfo{};
  auto options = client::WriteOptions();
  options.retry().set_max_retry_time(10_s);

  std::vector<folly::SemiFuture<folly::Expected<folly::Unit, hf3fs::Status>>> writeTasks;

  for (auto &writeIO : writeIOs) {
    auto writeTask = storageClient_->write(writeIO, dummyUserInfo, options).scheduleOn(&requestExe_).start();
    writeTasks.push_back(std::move(writeTask));
  }

  // retry for a short time
  std::this_thread::sleep_for(1500_ms);
  // check the request failed with communication error
#if defined(__has_feature)
#if !__has_feature(thread_sanitizer)
  for (auto &writeIO : writeIOs) {
    ASSERT_FALSE(writeIO.result.lengthInfo);
    if (failedTargetIndex_ == 0) {
      switch (writeIO.result.lengthInfo.error().code()) {
        case StorageClientCode::kCommError:
        case StorageClientCode::kTimeout:
          break;
        default:
          ASSERT_TRUE(writeIO.result.lengthInfo.error().code());
      }
    } else
      ASSERT_EQ(StorageClientCode::kResourceBusy, writeIO.result.lengthInfo.error().code());
  }
#endif
#endif

  // set the target to offline state
  ASSERT_TRUE(updateRoutingInfo([&](auto &routingInfo) { setTargetOffline(routingInfo, failedTargetIndex_); }));

  // wait until all write IOs completed
  folly::coro::blockingWait(folly::coro::collectAllRange(std::move(writeTasks)));

  if (!storageServers_.empty()) {
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

  for (auto &writeIO : writeIOs) {
    if (storageServers_.empty()) {
      // only single replica
      ASSERT_TRUE(writeIO.result.lengthInfo.hasError());
      ASSERT_EQ(StorageClientCode::kNotAvailable, writeIO.result.lengthInfo.error().code());
    } else {
      // multiple replicas
      ASSERT_OK(writeIO.result.lengthInfo);
      ASSERT_EQ(chunkData.size(), writeIO.result.lengthInfo.value());

      // read and check chunk data
      std::vector<uint8_t> readData(chunkData.size());
      auto readRes = readFromChunk(chainId, chunkId, readData);
      ASSERT_OK(readRes.lengthInfo);
      ASSERT_EQ(numWriteIOs, readRes.commitVer);
      ASSERT_EQ(numWriteIOs, readRes.updateVer);
      ASSERT_EQ(chunkData.size(), *readRes.lengthInfo);
      ASSERT_EQ(chunkData, readData);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(SingleReplica,
                         TestStorageServiceFailStop,
                         ::testing::Combine(::testing::Values(1) /*numReplicas*/,
                                            ::testing::Values(0) /*failedTargetIndex*/,
                                            ::testing::Values(true /*useFakeMgmtdClient*/)),
                         prettyPrintConfig);

INSTANTIATE_TEST_SUITE_P(TwoReplicas,
                         TestStorageServiceFailStop,
                         ::testing::Combine(::testing::Values(2) /*numReplicas*/,
                                            ::testing::Values(0, 1) /*failedTargetIndex*/,
                                            ::testing::Values(true /*useFakeMgmtdClient*/)),
                         prettyPrintConfig);
INSTANTIATE_TEST_SUITE_P(ThreeReplicas,
                         TestStorageServiceFailStop,
                         ::testing::Combine(::testing::Values(3) /*numReplicas*/,
                                            ::testing::Values(0, 1, 2) /*failedTargetIndex*/,
                                            ::testing::Values(true /*useFakeMgmtdClient*/)),
                         prettyPrintConfig);

}  // namespace
}  // namespace hf3fs::storage
