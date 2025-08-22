#include <thread>

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

class TestSyncStart : public UnitTestFabric, public ::testing::Test {
 protected:
  TestSyncStart()
      : UnitTestFabric(SystemSetupConfig{128_KB /*chunkSize*/,
                                         1 /*numChains*/,
                                         1 /*numReplicas*/,
                                         1 /*numStorageNodes*/,
                                         {folly::fs::temp_directory_path()} /*dataPaths*/,
                                         hf3fs::Path() /*clientConfig*/,
                                         hf3fs::Path() /*serverConfig*/,
                                         {} /*storageEndpoints*/,
                                         0 /*serviceLevel*/,
                                         0 /*listenPort*/,
                                         client::StorageClient::ImplementationType::RPC,
                                         kv::KVStore::Type::RocksDB,
                                         false,
                                         true}) {}

  void SetUp() override {
    // init ib device
    net::IBDevice::Config ibConfig;
    auto ibResult = net::IBManager::start(ibConfig);
    ASSERT_OK(ibResult);
    ASSERT_TRUE(setUpStorageSystem());
  }

  void TearDown() override { tearDownStorageSystem(); }
};

TEST_F(TestSyncStart, DISABLED_Normal) {
  net::Client::Config config;
  client::StorageMessenger messenger(config);
  ASSERT_TRUE(messenger.start());

  auto addr = storageServers_.back()->groups().front()->addressList().front();

  std::optional<hf3fs::flat::ChainInfo> firstChain;
  updateRoutingInfo([&](auto &routingInfo) {
    auto *chainTable = routingInfo.getChainTable(kTableId());
    firstChain = routingInfo.chains[chainTable->chains.front()];

    for (auto &[id, chain] : routingInfo.chains) {
      chain.targets.back().publicState = flat::PublicTargetState::SYNCING;
      chain.chainVersion = flat::ChainVersion{2};
      routingInfo.targets[chain.targets.back().targetId].localState = flat::LocalTargetState::ONLINE;
      routingInfo.targets[chain.targets.back().targetId].publicState = flat::PublicTargetState::SYNCING;
    }
  });
  std::this_thread::sleep_for(1000_ms);

  SyncStartReq syncStartReq;
  syncStartReq.vChainId.chainId = firstChain->chainId;
  syncStartReq.vChainId.chainVer = firstChain->chainVersion;
  auto syncStartResult = folly::coro::blockingWait(messenger.syncStart(addr, syncStartReq));
  ASSERT_OK(syncStartResult);

  SyncDoneReq syncDoneReq;
  syncDoneReq.vChainId.chainId = firstChain->chainId;
  syncDoneReq.vChainId.chainVer = firstChain->chainVersion;
  auto syncDoneResult = folly::coro::blockingWait(messenger.syncDone(addr, syncDoneReq));
  ASSERT_OK(syncDoneResult);
}

}  // namespace
}  // namespace hf3fs::storage::test
