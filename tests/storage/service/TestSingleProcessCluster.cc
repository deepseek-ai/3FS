#include <filesystem>
#include <folly/experimental/coro/Collect.h>

#include "common/serde/Serde.h"
#include "common/utils/FileUtils.h"
#include "common/utils/SysResource.h"
#include "tests/GtestHelpers.h"
#include "tests/lib/Helper.h"
#include "tests/lib/UnitTestFabric.h"

namespace hf3fs::storage {
namespace {

using namespace hf3fs::test;

class TestSingleProcessCluster : public UnitTestFabric, public ::testing::TestWithParam<SystemSetupConfig> {
 protected:
  TestSingleProcessCluster()
      : UnitTestFabric(GetParam()) {}

  void SetUp() override {
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    GTEST_SKIP();
#endif
#endif

    // init ib device
    net::IBDevice::Config ibConfig;
    auto ibResult = net::IBManager::start(ibConfig);
    ASSERT_OK(ibResult);

    ASSERT_TRUE(setUpStorageSystem());

    for (auto nodeId : storageNodeIds_) {
      waitStorageNodeStatus({nodeId}, flat::NodeStatus::HEARTBEAT_CONNECTED);
      waitStorageTargetStatus(nodeTargets_[nodeId], flat::LocalTargetState::UPTODATE, flat::PublicTargetState::SERVING);
    }

    mgmtdServer_.config.service().set_check_status_interval(200_ms);
    mgmtdServer_.config.service().set_heartbeat_fail_interval(1_s);

    clientConfig_.retry().set_init_wait_time(200_ms);
    clientConfig_.retry().set_max_wait_time(5_s);
    clientConfig_.retry().set_max_retry_time(60_s);
    ASSERT_LT(clientConfig_.retry().init_wait_time(), clientConfig_.retry().max_wait_time());
  }

  void TearDown() override { tearDownStorageSystem(); }

  void checkStorageNodeStatus(const std::vector<flat::NodeId> &nodeIds, flat::NodeStatus status) {
    for (const auto &nodeId : nodeIds) {
      ASSERT_TRUE(nodeTargets_.count(nodeId));
      auto nodeInfo = rawRoutingInfo_->getNode(nodeId);
      ASSERT_NE(nullptr, nodeInfo);
      ASSERT_EQ(status, nodeInfo->status);
    }
  }

  void checkStorageTargetStatus(const std::vector<flat::TargetId> &targetIds,
                                flat::LocalTargetState localStatus,
                                flat::PublicTargetState publicStatus) {
    for (const auto &targetId : targetIds) {
      auto targetInfo = rawRoutingInfo_->getTarget(targetId);
      ASSERT_NE(nullptr, targetInfo);
      ASSERT_EQ(localStatus, targetInfo->localState);
      ASSERT_EQ(publicStatus, targetInfo->publicState);
    }
  }

  void waitStorageNodeStatus(const std::vector<flat::NodeId> &nodeIds,
                             flat::NodeStatus status,
                             size_t maxRetries = 200) {
    flat::NodeInfo *nodeInfo = nullptr;

    XLOGF(INFO,
          "# Waiting storage nodes {} moving to status {}",
          serde::toJsonString(nodeIds),
          magic_enum::enum_name(status));

    for (size_t retry = 0; retry < maxRetries; retry++) {
      if (retry > 0) std::this_thread::sleep_for(mgmtdServer_.config.service().check_status_interval());

      auto routingInfo = getRoutingInfo();

      if (routingInfo) {
        bool allMatch = true;

        for (const auto &nodeId : nodeIds) {
          ASSERT_TRUE(nodeTargets_.count(nodeId));
          nodeInfo = routingInfo->getNode(nodeId);
          ASSERT_NE(nullptr, nodeInfo);
          if (nodeInfo->status != status) {
            XLOGF(INFO,
                  "#{} Waiting storage node {} moving to status {}, current status {}, routing info: {}",
                  retry,
                  nodeId,
                  magic_enum::enum_name(status),
                  magic_enum::enum_name(nodeInfo->status),
                  routingInfo->routingInfoVersion);
            allMatch = false;
            break;
          }
        }

        if (allMatch) return;
      }
    }

    ASSERT_NE(nullptr, nodeInfo);
    ASSERT_EQ(status, nodeInfo->status) << "node id " << nodeInfo->app.nodeId;
  }

  void waitStorageTargetStatus(const std::vector<flat::TargetId> &targetIds,
                               flat::LocalTargetState localStatus,
                               flat::PublicTargetState publicStatus,
                               size_t maxRetries = 500) {
    flat::TargetInfo *targetInfo = nullptr;

    XLOGF(INFO,
          "# wait storage targets {} moving to status {}/{}",
          serde::toJsonString(targetIds),
          magic_enum::enum_name(localStatus),
          magic_enum::enum_name(publicStatus));

    for (size_t retry = 0; retry < maxRetries; retry++) {
      if (retry > 0) std::this_thread::sleep_for(mgmtdServer_.config.service().check_status_interval());

      auto routingInfo = getRoutingInfo();

      if (routingInfo) {
        bool allMatch = true;

        for (const auto &targetId : targetIds) {
          targetInfo = routingInfo->getTarget(targetId);
          ASSERT_NE(nullptr, targetInfo);
          if (localStatus != targetInfo->localState || publicStatus != targetInfo->publicState) {
            XLOGF(INFO,
                  "#{} Waiting storage target {} moving to status {}/{}, current status {}/{}, routing info: {}",
                  retry,
                  targetId,
                  magic_enum::enum_name(localStatus),
                  magic_enum::enum_name(publicStatus),
                  magic_enum::enum_name(targetInfo->localState),
                  magic_enum::enum_name(targetInfo->publicState),
                  routingInfo->routingInfoVersion);
            allMatch = false;
            break;
          }
        }

        if (allMatch) return;
      }
    }

    ASSERT_NE(nullptr, targetInfo);
    ASSERT_EQ(localStatus, targetInfo->localState) << "target id " << targetInfo->targetId;
    ASSERT_EQ(publicStatus, targetInfo->publicState) << "target id " << targetInfo->targetId;
  }

  void readAndCompareAllReplicas(const ChainId &chainId,
                                 const ChunkId &chunkBegin,
                                 size_t numChunks,
                                 const std::vector<IOResult> &expectedResults) {
    const ChunkId chunkEnd(chunkBegin, numChunks);
    std::vector<std::vector<std::vector<uint8_t>>> chunkData(setupConfig_.num_replicas());
    std::vector<std::vector<IOResult>> ioResults(setupConfig_.num_replicas());

    for (size_t replicaIndex = 0; replicaIndex < setupConfig_.num_replicas(); replicaIndex++) {
      client::ReadOptions options;
      options.targetSelection().set_mode(client::TargetSelectionMode::ManualMode);
      options.targetSelection().set_targetIndex(replicaIndex);

      auto readRes = readFromChunks(chainId,
                                    chunkBegin,
                                    chunkEnd,
                                    chunkData[replicaIndex],
                                    0,
                                    setupConfig_.chunk_size(),
                                    options,
                                    &ioResults[replicaIndex]);
      ASSERT_TRUE(readRes);
    }

    for (size_t chunkIndex = 0; chunkIndex < numChunks; chunkIndex++) {
      for (size_t replicaIndex = 0; replicaIndex < setupConfig_.num_replicas(); replicaIndex++) {
        ASSERT_EQ(chunkData[0][chunkIndex], chunkData[replicaIndex][chunkIndex]);
        ASSERT_EQ(expectedResults[chunkIndex].commitVer, ioResults[replicaIndex][chunkIndex].commitVer);
        ASSERT_EQ(expectedResults[chunkIndex].updateVer, ioResults[replicaIndex][chunkIndex].updateVer);
        ASSERT_EQ(expectedResults[chunkIndex].checksum, ioResults[replicaIndex][chunkIndex].checksum);
        ASSERT_EQ(expectedResults[chunkIndex].commitChainVer, ioResults[replicaIndex][chunkIndex].commitChainVer);
      }
    }
  }
};

TEST_P(TestSingleProcessCluster, StorageFailureDetected) {
  // stop the first storage server
  flat::NodeId firstNodeId{1};
  stopAndRemoveStorageServer(firstNodeId);

  waitStorageTargetStatus(nodeTargets_[firstNodeId], flat::LocalTargetState::OFFLINE, flat::PublicTargetState::OFFLINE);
  waitStorageNodeStatus({firstNodeId}, flat::NodeStatus::HEARTBEAT_FAILED);
}

TEST_P(TestSingleProcessCluster, StorageFailureDetectedBeforeWrite) {
  // stop the first storage server
  flat::NodeId firstNodeId{1};
  stopAndRemoveStorageServer(firstNodeId);

  waitStorageTargetStatus(nodeTargets_[firstNodeId], flat::LocalTargetState::OFFLINE, flat::PublicTargetState::OFFLINE);

  size_t numChunks = 10;
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);

  for (const auto &chainId : chainIds_) {
    storage::ChunkId chunkBegin(1, 0);
    storage::ChunkId chunkEnd(1, numChunks);
    auto writeRes = writeToChunks(chainId, chunkBegin, chunkEnd, chunkData);
    ASSERT_TRUE(writeRes);
  }
}

#if false
TEST_P(TestSingleProcessCluster, DISABLED_StorageWriteFailed) {
  clientConfig_.retry().set_init_wait_time(1_s);
  clientConfig_.retry().set_max_wait_time(1_s);
  clientConfig_.retry().set_max_retry_time(5_s);

  std::vector<uint8_t> chunkData(setupConfig_.chunk_size(), 0xFF);
  auto writeRes = writeToChunk(chainIds_.front(), ChunkId{0u, 0u}, chunkData);
  ASSERT_OK(writeRes.lengthInfo);

  auto flagPath = fmt::format("/tmp/storage_main_write_failed.{}", SysResource::pid());
  ASSERT_OK(hf3fs::storeToFile(flagPath, "100"));
  auto guard = folly::makeGuard([&] { std::filesystem::remove(flagPath); });

  // write the special chunk and receive fail.
  writeRes = writeToChunk(chainIds_.front(), ChunkId{0u, 0u}, chunkData);
  ASSERT_FALSE(writeRes.lengthInfo);

  waitStorageTargetStatus(nodeTargets_[flat::NodeId{1}],
                          flat::LocalTargetState::OFFLINE,
                          flat::PublicTargetState::OFFLINE);
}
#endif

TEST_P(TestSingleProcessCluster, StorageFailAndRestart) {
  // stop the first storage server
  uint32_t nodeIndex = 0;
  auto firstNodeId = storageNodeIds_[nodeIndex];
  stopAndRemoveStorageServer(nodeIndex);

  waitStorageNodeStatus({firstNodeId}, flat::NodeStatus::HEARTBEAT_FAILED);
  waitStorageTargetStatus(nodeTargets_[firstNodeId], flat::LocalTargetState::OFFLINE, flat::PublicTargetState::OFFLINE);

  size_t numChunks = 10;
  storage::ChunkId chunkBegin(1, 0);
  storage::ChunkId chunkEnd(1, numChunks);
  std::vector<std::vector<IOResult>> writeResults(chainIds_.size());

  for (size_t chainIndex = 0; chainIndex < chainIds_.size(); chainIndex++) {
    uint32_t offset = folly::Random::rand32(0, setupConfig_.chunk_size());
    std::vector<uint8_t> chunkData(folly::Random::rand32(1, setupConfig_.chunk_size() - offset + 1),
                                   (uint8_t)folly::Random::rand32());
    auto writeRes = writeToChunks(chainIds_[chainIndex],
                                  chunkBegin,
                                  chunkEnd,
                                  chunkData,
                                  offset,
                                  chunkData.size(),
                                  client::WriteOptions(),
                                  &writeResults[chainIndex]);
    ASSERT_TRUE(writeRes);
  }

  // restart storage server
  auto storageServer = createStorageServer(nodeIndex);
  ASSERT_NE(storageServer, nullptr);
  storageServers_.insert(storageServers_.begin() + nodeIndex, std::move(storageServer));

  waitStorageNodeStatus({firstNodeId}, flat::NodeStatus::HEARTBEAT_CONNECTED);
  waitStorageTargetStatus(nodeTargets_[firstNodeId],
                          flat::LocalTargetState::UPTODATE,
                          flat::PublicTargetState::SERVING);

  // read chunk data from all replicas
  for (size_t chainIndex = 0; chainIndex < chainIds_.size(); chainIndex++) {
    readAndCompareAllReplicas(chainIds_[chainIndex], chunkBegin, numChunks, writeResults[chainIndex]);
  }
}

TEST_P(TestSingleProcessCluster, StorageRepeatedFailAndRestart) {
  uint32_t maxLoops = setupConfig_.num_storage_nodes();

  for (uint32_t testLoop = 1; testLoop <= maxLoops; testLoop++) {
    uint64_t chunkIdPrefix = testLoop;
    size_t numChunks = 10;
    storage::ChunkId chunkBegin(chunkIdPrefix, 0);
    storage::ChunkId chunkEnd(chunkIdPrefix, numChunks);
    std::vector<std::vector<IOResult>> writeResults(chainIds_.size());

    uint32_t nodeIndex = folly::Random::rand32(0, setupConfig_.num_storage_nodes());
    auto nodeId = storageNodeIds_[nodeIndex];
    XLOGF(INFO, "Test loop {}, stopping #{} {}", testLoop, nodeIndex, nodeId);
    stopAndRemoveStorageServer(nodeIndex);

    waitStorageNodeStatus({nodeId}, flat::NodeStatus::HEARTBEAT_FAILED, 100 /*maxRetries*/);
    waitStorageTargetStatus(nodeTargets_[nodeId],
                            flat::LocalTargetState::OFFLINE,
                            flat::PublicTargetState::OFFLINE,
                            500 /*maxRetries*/);

    XLOGF(INFO, "Test loop {}, writing chunks in range {} - {}", testLoop, chunkBegin, chunkEnd);

    for (size_t chainIndex = 0; chainIndex < chainIds_.size(); chainIndex++) {
      uint32_t offset = folly::Random::rand32(0, setupConfig_.chunk_size());
      std::vector<uint8_t> chunkData(folly::Random::rand32(1, setupConfig_.chunk_size() - offset + 1),
                                     (uint8_t)folly::Random::rand32());
      auto writeRes = writeToChunks(chainIds_[chainIndex],
                                    chunkBegin,
                                    chunkEnd,
                                    chunkData,
                                    offset,
                                    chunkData.size(),
                                    client::WriteOptions(),
                                    &writeResults[chainIndex]);
      ASSERT_TRUE(writeRes);
    }

    // restart storage server
    XLOGF(INFO, "Test loop {}, restarting #{} {}", testLoop, nodeIndex, nodeId);
    auto storageServer = createStorageServer(nodeIndex);
    ASSERT_NE(storageServer, nullptr);
    storageServers_.insert(storageServers_.begin() + nodeIndex, std::move(storageServer));

    waitStorageNodeStatus({nodeId}, flat::NodeStatus::HEARTBEAT_CONNECTED, 200 /*maxRetries*/);
    waitStorageTargetStatus(nodeTargets_[nodeId],
                            flat::LocalTargetState::UPTODATE,
                            flat::PublicTargetState::SERVING,
                            1000 /*maxRetries*/);

    XLOGF(INFO, "Test loop {}, reading chunks in range {} - {}", testLoop, chunkBegin, chunkEnd);

    // read chunk data from all replicas
    for (size_t chainIndex = 0; chainIndex < chainIds_.size(); chainIndex++) {
      readAndCompareAllReplicas(chainIds_[chainIndex], chunkBegin, numChunks, writeResults[chainIndex]);
    }
  }
}

TEST_P(TestSingleProcessCluster, MultiStoragesRepeatedFailAndRestart) {
  uint32_t nodeIndexStart = folly::Random::rand32(0, setupConfig_.num_storage_nodes());
  uint32_t maxLoops = setupConfig_.num_storage_nodes();

  for (uint32_t testLoop = 1; testLoop <= maxLoops; testLoop++) {
    uint64_t chunkIdPrefix = testLoop;
    size_t numChunks = 10;
    storage::ChunkId chunkBegin(chunkIdPrefix, 0);
    storage::ChunkId chunkEnd(chunkIdPrefix, numChunks);

    uint32_t nodeIndex = (nodeIndexStart + testLoop) % setupConfig_.num_storage_nodes();
    auto nodeId = storageNodeIds_[nodeIndex];

    auto routingInfo = getRoutingInfo();
    ASSERT_NE(nullptr, routingInfo);
    auto nodeInfo = routingInfo->getNode(nodeId);
    ASSERT_NE(nullptr, nodeInfo);

    bool stoppedStorage = false;
    bool writeFailed = false;

    if (nodeInfo->status == flat::NodeStatus::HEARTBEAT_CONNECTED) {
      XLOGF(INFO, "Test loop {}, stopping #{} {}", testLoop, nodeIndex, nodeId);
      stoppedStorage = stopAndRemoveStorageServer(nodeId);
    }

    XLOGF(INFO, "Test loop {}, writing chunks in range {} - {}", testLoop, chunkBegin, chunkEnd);

    for (const auto &chainId : chainIds_) {
      uint32_t offset = folly::Random::rand32(0, setupConfig_.chunk_size());
      std::vector<uint8_t> chunkData(folly::Random::rand32(1, setupConfig_.chunk_size() - offset + 1),
                                     (uint8_t)folly::Random::rand32());
      auto writeRes = writeToChunks(chainId, chunkBegin, chunkEnd, chunkData, offset, chunkData.size());

      auto routingInfo = getRoutingInfo();
      auto chainInfo = routingInfo->getChain(chainId);
      ASSERT_NE(nullptr, chainInfo);

      if (chainInfo->targets.front().publicState == flat::PublicTargetState::SERVING) {
        // check write result if the chain has serving target
        ASSERT_TRUE(writeRes);
      } else {
        XLOGF(INFO, "Test loop {}, failed to write to chain: {:?}", testLoop, *chainInfo);
        writeFailed = true;
        break;
      }
    }

    // restart storage server
    if (stoppedStorage) {
      XLOGF(INFO, "Test loop {}, restarting #{} {}", testLoop, nodeIndex, nodeId);
      auto storageServer = createStorageServer(nodeIndex);
      ASSERT_NE(storageServer, nullptr);
      storageServers_.insert(storageServers_.begin() + nodeIndex, std::move(storageServer));
    }

    if (writeFailed) {
      // read chunks from last test loop if write in current loop failed
      chunkBegin = storage::ChunkId(chunkIdPrefix - 1, 0);
      chunkEnd = storage::ChunkId(chunkIdPrefix - 1, numChunks);
    }

    // read chunk data from all serving replicas
    XLOGF(INFO, "Test loop {}, reading chunks in range {} - {}", testLoop, chunkBegin, chunkEnd);

    for (const auto &chainId : chainIds_) {
      for (size_t retryIndex = 0;; retryIndex++) {
        XLOGF(INFO,
              "Test loop {}, #{} retry, reading chunks in range {} - {}",
              testLoop,
              retryIndex,
              chunkBegin,
              chunkEnd);

        std::vector<std::vector<uint8_t>> chunkData;
        storage::client::ReadOptions options;
        options.targetSelection().set_mode(storage::client::TargetSelectionMode::RoundRobin);
        auto readRes = readFromChunks(chainId, chunkBegin, chunkEnd, chunkData, 0, setupConfig_.chunk_size(), options);

        auto chainInfo = routingInfo->getChain(chainId);
        ASSERT_NE(nullptr, chainInfo);

        if (chainInfo->targets.front().publicState == flat::PublicTargetState::SERVING) {
          // check read result if the chain has serving target
          ASSERT_TRUE(readRes);
          break;
        } else {
          // otherwise wait the storage server restart and retry
          waitStorageNodeStatus({nodeId}, flat::NodeStatus::HEARTBEAT_CONNECTED, 200 /*maxRetries*/);
          waitStorageTargetStatus(nodeTargets_[nodeId],
                                  flat::LocalTargetState::UPTODATE,
                                  flat::PublicTargetState::SERVING,
                                  1000 /*maxRetries*/);
        }
      }
    }
  }
}

TEST_P(TestSingleProcessCluster, ConcurrentStorageSyncAndWrite) {
  uint32_t nodeIndexStart = folly::Random::rand32(0, setupConfig_.num_storage_nodes());
  uint32_t maxLoops = setupConfig_.num_storage_nodes();

  for (uint32_t testLoop = 1; testLoop <= maxLoops; testLoop++) {
    uint64_t chunkIdPrefix = testLoop;
    size_t numChunks = 20;
    storage::ChunkId chunkBegin(chunkIdPrefix, 0);
    storage::ChunkId chunkEnd(chunkIdPrefix, numChunks);

    uint32_t nodeIndex = (nodeIndexStart + testLoop) % setupConfig_.num_storage_nodes();
    auto nodeId = storageNodeIds_[nodeIndex];

    XLOGF(INFO, "Test loop {}, writing chunks in range {} - {}", testLoop, chunkBegin, chunkEnd);
    std::atomic<bool> runWrite = true;
    std::atomic<bool> writeOK = true;

    std::vector<std::jthread> backgroundWorkers;
    backgroundWorkers.reserve(chainIds_.size());
    std::vector<std::vector<IOResult>> writeResults(chainIds_.size());

    for (size_t chainIndex = 0; chainIndex < chainIds_.size(); chainIndex++) {
      const auto &chainId = chainIds_[chainIndex];
      auto &ioResults = writeResults[chainIndex];
      backgroundWorkers.emplace_back([&, this]() {
        for (uint32_t chunkVer = 1; runWrite && writeOK; chunkVer++) {
          uint32_t offset = folly::Random::rand32(0, setupConfig_.chunk_size());
          std::vector<uint8_t> chunkData(folly::Random::rand32(1, setupConfig_.chunk_size() - offset + 1),
                                         (uint8_t)folly::Random::rand32());

          ioResults.clear();
          auto writeRes = writeToChunks(chainId,
                                        chunkBegin,
                                        chunkEnd,
                                        chunkData,
                                        offset,
                                        chunkData.size(),
                                        client::WriteOptions(),
                                        &ioResults);
          if (!writeRes) writeOK = false;
          ASSERT_TRUE(writeRes);

          for (const auto &result : ioResults) {
            writeRes = result.lengthInfo && *result.lengthInfo == chunkData.size() && chunkVer == result.commitVer;
            if (!writeRes) writeOK = false;
            ASSERT_TRUE(writeRes);
          }
        }
      });
    }

    // write a few chunks before restart
    auto waitTime = mgmtdServer_.config.service().heartbeat_fail_interval() * 2;
    std::this_thread::sleep_for(waitTime);
    ASSERT_TRUE(writeOK);

    auto routingInfo = getRoutingInfo();
    ASSERT_NE(nullptr, routingInfo);
    auto nodeInfo = routingInfo->getNode(nodeId);
    ASSERT_NE(nullptr, nodeInfo);

    if (nodeInfo->status == flat::NodeStatus::HEARTBEAT_CONNECTED) {
      XLOGF(INFO, "Test loop {}, try to stop #{} {}", testLoop, nodeIndex, nodeId);
      auto stoppedStorage = stopAndRemoveStorageServer(nodeId);
      ASSERT_TRUE(writeOK);

      if (stoppedStorage) {
        if (folly::Random::randBool(0.5)) {  // wait until mgmtd knows the failure
          waitStorageNodeStatus({nodeId}, flat::NodeStatus::HEARTBEAT_FAILED);
          XLOGF(INFO, "Test loop {}, stopped #{} {}", testLoop, nodeIndex, nodeId);
          ASSERT_TRUE(writeOK);
        }

        // restart the storage service
        XLOGF(INFO, "Test loop {}, try to start #{} {}", testLoop, nodeIndex, nodeId);
        auto storageServer = createStorageServer(nodeIndex);
        ASSERT_NE(storageServer, nullptr);
        storageServers_.insert(storageServers_.begin() + nodeIndex, std::move(storageServer));
        ASSERT_TRUE(writeOK);

        waitStorageNodeStatus({nodeId}, flat::NodeStatus::HEARTBEAT_CONNECTED);
        XLOGF(INFO, "Test loop {}, started #{} {}", testLoop, nodeIndex, nodeId);
        ASSERT_TRUE(writeOK);
      }
    }

    std::this_thread::sleep_for(waitTime);
    ASSERT_TRUE(writeOK);

    runWrite = false;
    backgroundWorkers.clear();
    ASSERT_TRUE(writeOK);

    // read chunk data from the serving replicas

    for (const auto &chainId : chainIds_) {
      XLOGF(INFO, "Test loop {}, chain {}, reading chunks in range {} - {}", testLoop, chainId, chunkBegin, chunkEnd);
      std::vector<std::vector<uint8_t>> chunkData;
      auto readRes = readFromChunks(chainId, chunkBegin, chunkEnd, chunkData, 0, setupConfig_.chunk_size());
      ASSERT_TRUE(readRes);
    }

    waitStorageNodeStatus({nodeId}, flat::NodeStatus::HEARTBEAT_CONNECTED, 200 /*maxRetries*/);
    waitStorageTargetStatus(nodeTargets_[nodeId],
                            flat::LocalTargetState::UPTODATE,
                            flat::PublicTargetState::SERVING,
                            1000 /*maxRetries*/);

    // read chunk data from all replicas
    for (size_t chainIndex = 0; chainIndex < chainIds_.size(); chainIndex++) {
      readAndCompareAllReplicas(chainIds_[chainIndex], chunkBegin, numChunks, writeResults[chainIndex]);
    }
  }
}

TEST_P(TestSingleProcessCluster, StorageResetUncommittedChunks) {
  size_t numChunks = 5;
  std::vector<uint8_t> chunkData(setupConfig_.chunk_size() - 1, 0x86);
  auto chainId = chainIds_.front();

  auto regRes = storageClient_->registerIOBuffer(&chunkData[0], chunkData.size());
  ASSERT_OK(regRes);
  auto ioBuffer = std::move(*regRes);

  // create write IOs
  std::vector<client::WriteIO> writeIOs;
  for (size_t i = 0; i < numChunks; ++i) {
    auto writeIO = storageClient_->createWriteIO(chainId,
                                                 ChunkId{1, i},
                                                 1 /*offset*/,
                                                 chunkData.size() /*length*/,
                                                 setupConfig_.chunk_size() /*chunkSize*/,
                                                 &chunkData[0],
                                                 &ioBuffer);
    writeIOs.push_back(std::move(writeIO));
  }

  clientConfig_.retry().set_init_wait_time(1_s);
  clientConfig_.retry().set_max_wait_time(1_s);
  clientConfig_.retry().set_max_retry_time(1_s);
  flat::UserInfo dummyUserInfo{};
  std::vector<folly::SemiFuture<folly::Expected<folly::Unit, hf3fs::Status>>> ioTasks;
  storage::client::WriteOptions options;
  for (auto &writeIO : writeIOs) {
    auto task = storageClient_->write(writeIO, dummyUserInfo, options).scheduleOn(&requestExe_).start();
    ioTasks.push_back(std::move(task));
  }

  // stop tail server.
  auto lastNodeId = storageNodeIds_.back();
  stopAndRemoveStorageServer(lastNodeId);
  std::this_thread::sleep_for(100_ms);

  // restart head server.
  folly::coro::blockingWait(folly::coro::collectAllRange(std::move(ioTasks)));
  auto succ = std::count_if(writeIOs.begin(), writeIOs.end(), [](auto &w) { return bool(w.result.lengthInfo); });
  XLOGF(INFO, "write chunks succ: {}", succ);
  ASSERT_LE(succ, numChunks);
  auto firstNodeId = storageNodeIds_.front();
  stopAndRemoveStorageServer(firstNodeId);

  auto storageServer = createStorageServer(0);
  ASSERT_NE(storageServer, nullptr);
  storageServers_.insert(storageServers_.end(), std::move(storageServer));
  waitStorageTargetStatus(nodeTargets_[flat::NodeId{1}],
                          flat::LocalTargetState::UPTODATE,
                          flat::PublicTargetState::SERVING,
                          1000);

  clientConfig_.retry().set_init_wait_time(1_s);
  clientConfig_.retry().set_max_wait_time(1_s);
  clientConfig_.retry().set_max_retry_time(5_s);
  std::vector<std::vector<uint8_t>> readResult;
  ASSERT_TRUE(readFromChunks(chainId, ChunkId{1, 0}, ChunkId{1, numChunks}, readResult, 1, chunkData.size()));
  ASSERT_EQ(readResult.size(), numChunks);
  for (auto &line : readResult) {
    ASSERT_EQ(line, chunkData);
  }

  // write again.
  ASSERT_TRUE(writeToChunks(chainId, ChunkId{1, 0}, ChunkId{1, numChunks}, chunkData));
}

SystemSetupConfig testTwoReplicas = {
    32_KB /*chunkSize*/,
    16 /*numChains*/,
    2 /*numReplicas*/,
    2 /*numStorageNodes*/,
    {folly::fs::temp_directory_path()} /*dataPaths*/,
    "" /*clientConfig*/,
    "" /*serverConfig*/,
    {} /*storageEndpoints*/,
    0 /*serviceLevel*/,
    0 /*listenPort*/,
    storage::client::StorageClient::ImplementationType::RPC /*clientImplType*/,
    kv::KVStore::Type::LevelDB /*metaStoreType*/,
    false /*useFakeMgmtdClient*/,
    true /*startStorageServer*/,
};

SystemSetupConfig testThreeReplicas = {
    32_KB /*chunkSize*/,
    16 /*numChains*/,
    3 /*numReplicas*/,
    3 /*numStorageNodes*/,
    {folly::fs::temp_directory_path()} /*dataPaths*/,
    "" /*clientConfig*/,
    "" /*serverConfig*/,
    {} /*storageEndpoints*/,
    0 /*serviceLevel*/,
    0 /*listenPort*/,
    storage::client::StorageClient::ImplementationType::RPC /*clientImplType*/,
    kv::KVStore::Type::LevelDB /*metaStoreType*/,
    false /*useFakeMgmtdClient*/,
    true /*startStorageServer*/,
};

INSTANTIATE_TEST_SUITE_P(TwoReplicas,
                         TestSingleProcessCluster,
                         ::testing::Values(testTwoReplicas),
                         SystemSetupConfig::prettyPrintConfig);

INSTANTIATE_TEST_SUITE_P(ThreeReplicas,
                         TestSingleProcessCluster,
                         ::testing::Values(testThreeReplicas),
                         SystemSetupConfig::prettyPrintConfig);

}  // namespace
}  // namespace hf3fs::storage
