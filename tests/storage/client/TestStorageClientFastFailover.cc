#include <folly/experimental/coro/BlockingWait.h>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/net/Client.h"
#include "tests/GtestHelpers.h"
#include "tests/lib/UnitTestFabric.h"

namespace hf3fs::storage::client {
namespace {

using namespace hf3fs::test;

class TestStorageClientFastFailover : public UnitTestFabric, public ::testing::TestWithParam<SystemSetupConfig> {
 protected:
  TestStorageClientFastFailover()
      : UnitTestFabric(GetParam()) {}

  void SetUp() override {
    // init ib device
    net::IBDevice::Config ibConfig;
    auto ibResult = net::IBManager::start(ibConfig);
    ASSERT_OK(ibResult);
    ASSERT_TRUE(setUpStorageSystem());

    // increase timeout to avoid comm error during data generation
    auto retryOptions = clientConfig_.retry();
    clientConfig_.retry().set_max_retry_time(5_s);

    numChunks_ = setupConfig_.num_replicas() * 10;
    std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);
    auto result = writeToChunks(firstChainId_, ChunkId(1, 0), ChunkId(1, numChunks_), chunkData);
    ASSERT_TRUE(result);
    XLOGF(INFO, "Created {} test chunks", numChunks_);
  }

  void TearDown() override { tearDownStorageSystem(); }

 protected:
  size_t numChunks_;
};

TEST_P(TestStorageClientFastFailover, ReadChunks) {
  client::ReadOptions options;
  // since we have much more chunks than replicas: numChunks_ = setupConfig_.num_replicas() * 10,
  // all replicas would be used in round robin mode
  options.targetSelection().set_mode(client::TargetSelectionMode::RoundRobin);

  std::vector<std::vector<uint8_t>> chunkData;
  auto result = readFromChunks(firstChainId_,
                               ChunkId(1, 0),
                               ChunkId(1, numChunks_),
                               chunkData,
                               0 /*offset*/,
                               setupConfig_.chunk_size() /*length*/,
                               options);
  ASSERT_TRUE(result);

  // stop the head target
  ASSERT_TRUE(stopAndRemoveStorageServer(0));

  result = readFromChunks(firstChainId_,
                          ChunkId(1, 0),
                          ChunkId(1, numChunks_),
                          chunkData,
                          0 /*offset*/,
                          setupConfig_.chunk_size() /*length*/,
                          options);

  if (setupConfig_.num_replicas() > 1) {
    ASSERT_TRUE(result);
  } else {
    ASSERT_FALSE(result);
  }
}

SystemSetupConfig singleReplica = {
    128_KB /*chunkSize*/,
    1 /*numChains*/,
    1 /*numReplicas*/,
    1 /*numStorageNodes*/,
    {folly::fs::temp_directory_path()} /*dataPaths*/,
};

SystemSetupConfig twoReplicas = {
    128_KB /*chunkSize*/,
    1 /*numChains*/,
    2 /*numReplicas*/,
    2 /*numStorageNodes*/,
    {folly::fs::temp_directory_path()} /*dataPaths*/,
};

SystemSetupConfig threeReplicas = {
    128_KB /*chunkSize*/,
    1 /*numChains*/,
    3 /*numReplicas*/,
    3 /*numStorageNodes*/,
    {folly::fs::temp_directory_path()} /*dataPaths*/,
};

INSTANTIATE_TEST_SUITE_P(SingleReplica,
                         TestStorageClientFastFailover,
                         ::testing::Values(singleReplica),
                         SystemSetupConfig::prettyPrintConfig);
INSTANTIATE_TEST_SUITE_P(TwoReplicas,
                         TestStorageClientFastFailover,
                         ::testing::Values(twoReplicas),
                         SystemSetupConfig::prettyPrintConfig);
INSTANTIATE_TEST_SUITE_P(ThreeReplicas,
                         TestStorageClientFastFailover,
                         ::testing::Values(threeReplicas),
                         SystemSetupConfig::prettyPrintConfig);

}  // namespace
}  // namespace hf3fs::storage::client
