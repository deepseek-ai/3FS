#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/net/Client.h"
#include "migration/service/Server.h"
#include "tests/lib/Helper.h"
#include "tests/lib/UnitTestFabric.h"

namespace hf3fs::migration {

using namespace hf3fs::test;

class TestTestMigrationService : public UnitTestFabric, public ::testing::Test {
 protected:
  TestTestMigrationService()
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
            storage::client::StorageClient::ImplementationType::RPC,
            kv::KVStore::Type::RocksDB,
            false /*useFakeMgmtdClient*/,
        }) {}

  void SetUp() override {
    // init ib device
    net::IBDevice::Config ibConfig;
    auto ibResult = net::IBManager::start(ibConfig);
    ASSERT_OK(ibResult);
    ASSERT_TRUE(setUpStorageSystem());
  }

  void TearDown() override { tearDownStorageSystem(); }
};

TEST_F(TestTestMigrationService, StartAndStopServer) {
  auto chainId = firstChainId_;
  storage::ChunkId chunkId(1 /*high*/, 1 /*low*/);
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);

  // create a test chunk
  auto ioResult = writeToChunk(chainId, chunkId, chunkData);
  ASSERT_RESULT_EQ(chunkData.size(), ioResult.lengthInfo);

  // get the first chain
  auto newRoutingInfo = copyRoutingInfo();
  auto &chainTable = *newRoutingInfo->getChainTable(kTableId());
  auto &firstChain = newRoutingInfo->chains[chainTable.chains.front()];
  ASSERT_EQ(chainId, firstChain.chainId);

  auto readRes = readFromChunk(chainId, chunkId, chunkData, 0, chunkData.size());
  ASSERT_OK(readRes.lengthInfo);
  auto writeRes = writeToChunk(chainId, chunkId, chunkData, 0, chunkData.size());
  ASSERT_OK(writeRes.lengthInfo);

  const auto &mgmtdAddressList = mgmtdServer_.collectAddressList("Mgmtd");

  server::MigrationServer::Config config;
  config.mgmtd_client().set_mgmtd_server_addresses(to_named(mgmtdAddressList));
  server::MigrationServer server(config);

  auto result = server.setup();
  ASSERT_OK(result);

  auto nodeEndpoint = server.groups().front()->addressList().front();
  XLOGF(WARN, "migration server address: {}", nodeEndpoint);

  hf3fs::flat::ServiceGroupInfo serviceGroupInfo;
  serviceGroupInfo.endpoints = {nodeEndpoint};

  flat::AppInfo appInfo;
  appInfo.clusterId = kClusterId;
  appInfo.serviceGroups.push_back(serviceGroupInfo);

  result = server.start(appInfo);
  ASSERT_OK(result);
  XLOGF(WARN, "migration server started: {}", nodeEndpoint);

  server.stopAndJoin();
  XLOGF(WARN, "migration server stopped: {}", nodeEndpoint);
}

}  // namespace hf3fs::migration
