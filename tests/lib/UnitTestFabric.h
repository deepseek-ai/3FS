#pragma once

#include <map>
#include <vector>

#include "client/mgmtd/MgmtdClientForClient.h"
#include "client/mgmtd/MgmtdClientForServer.h"
#include "client/storage/StorageClient.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "fbs/mgmtd/RoutingInfo.h"
#include "storage/service/StorageServer.h"
#include "tests/GtestHelpers.h"
#include "tests/client/ClientWithConfig.h"
#include "tests/client/ServerWithConfig.h"
#include "tests/mgmtd/MgmtdTestHelper.h"

namespace hf3fs::test {

class FakeMgmtdClient : public hf3fs::client::IMgmtdClientForClient, public hf3fs::client::IMgmtdClientForServer {
 public:
  FakeMgmtdClient(std::shared_ptr<hf3fs::flat::RoutingInfo> routingInfo) { setRoutingInfo(routingInfo); }

  std::shared_ptr<hf3fs::client::RoutingInfo> getRoutingInfo() override { return routingInfo_.load(); }

  void setRoutingInfo(std::shared_ptr<hf3fs::flat::RoutingInfo> routingInfo) {
    routingInfo_.store(std::make_shared<hf3fs::client::RoutingInfo>(routingInfo, SteadyClock::now()));
    for (auto &listener : listeners_) {
      listener(routingInfo_.load());
    }
  }

  CoTryTask<void> refreshRoutingInfo(bool) override { co_return Void{}; }

  CoTryTask<mgmtd::HeartbeatRsp> heartbeat() override { co_return makeError(StatusCode::kNotImplemented); }

  Result<Void> triggerHeartbeat() override { return makeError(StatusCode::kNotImplemented); }

  CoTryTask<std::optional<flat::ConfigInfo>> getConfig(flat::NodeType, flat::ConfigVersion) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<RHStringHashMap<flat::ConfigVersion>> getConfigVersions() override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<std::vector<flat::TagPair>> getUniversalTags(const String &universalId) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  void setAppInfoForHeartbeat(flat::AppInfo) override {}

  bool addRoutingInfoListener(String, RoutingInfoListener listener) override {
    listeners_.push_back(listener);
    return true;
  }

  bool removeRoutingInfoListener(std::string_view) override { return true; }

  void setConfigListener(ConfigListener) override {}

  void updateHeartbeatPayload(HeartbeatPayload) override {}

  CoTryTask<void> extendClientSession() override { co_return makeError(StatusCode::kNotImplemented); }

  void setClientSessionPayload(ClientSessionPayload) override {}

  CoTryTask<mgmtd::ListClientSessionsRsp> listClientSessions() override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<mgmtd::GetClientSessionRsp> getClientSession(const String &) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  // The diamond inheritance forbids directly upcasting from FakeMgmtdClient to ICommonMgmtdClient
  hf3fs::client::ICommonMgmtdClient *asCommon() {
    hf3fs::client::IMgmtdClientForServer *s = this;
    return s;
  }

 private:
  folly::atomic_shared_ptr<hf3fs::client::RoutingInfo> routingInfo_;
  std::vector<RoutingInfoListener> listeners_;
};

struct SystemSetupConfig : public ConfigBase<SystemSetupConfig> {
  CONFIG_ITEM(chunk_size, 128_KB);
  CONFIG_ITEM(num_chains, 1u);
  CONFIG_ITEM(num_replicas, 1u);
  CONFIG_ITEM(num_storage_nodes, 1u);
  CONFIG_ITEM(data_paths, (std::vector<hf3fs::Path>{}));
  CONFIG_ITEM(client_config, hf3fs::Path());
  CONFIG_ITEM(server_config, hf3fs::Path());
  CONFIG_ITEM(storage_endpoints, (std::map<std::string, net::Address>{}));
  CONFIG_ITEM(service_level, 0u);
  CONFIG_ITEM(listen_port, 0u);
  CONFIG_ITEM(client_impl_type, storage::client::StorageClient::ImplementationType::RPC);
  CONFIG_ITEM(meta_store_type, kv::KVStore::Type::LevelDB);
  CONFIG_ITEM(use_fake_mgmtd_client, true);
  CONFIG_ITEM(start_storage_server, true);
  CONFIG_ITEM(use_temp_path, true);

 public:
  std::vector<Size> getChunkSizeList() const {
    auto chunkSize = chunk_size();
    std::vector<Size> chunkSizeList;

    for (auto size : std::vector<Size>{chunkSize / 4, chunkSize / 2, chunkSize, 2 * chunkSize, 4 * chunkSize}) {
      if (size >= 512 && (size & size - 1) == 0 /*pow of 2*/) {
        chunkSizeList.push_back(size);
      }
    }

    return chunkSizeList;
  }

 public:
  SystemSetupConfig() = default;

  SystemSetupConfig(uint32_t chunkSize,
                    uint32_t numChains,
                    uint32_t numReplicas,
                    uint32_t numStorageNodes,
                    std::vector<hf3fs::Path> dataPaths,
                    hf3fs::Path clientConfig = hf3fs::Path(),
                    hf3fs::Path serverConfig = hf3fs::Path(),
                    std::map<storage::NodeId, net::Address> storageEndpoints = {},
                    uint32_t serviceLevel = 0,
                    uint32_t listenPort = 0,
                    storage::client::StorageClient::ImplementationType clientImplType =
                        storage::client::StorageClient::ImplementationType::RPC,
                    kv::KVStore::Type metaStoreType = kv::KVStore::Type::LevelDB,
                    bool useFakeMgmtdClient = true,
                    bool startStorageServer = true,
                    bool useTempPath = true) {
    set_chunk_size(chunkSize);
    set_num_chains(numChains);
    set_num_replicas(numReplicas);
    set_num_storage_nodes(numStorageNodes);
    set_data_paths(dataPaths);
    set_client_config(clientConfig);
    set_server_config(serverConfig);
    std::map<std::string, net::Address> map{};
    for (auto &[nodeId, addr] : storageEndpoints) {
      map[std::to_string(nodeId)] = addr;
    }
    set_storage_endpoints(map);
    set_service_level(serviceLevel);
    set_listen_port(listenPort);
    set_client_impl_type(clientImplType);
    set_meta_store_type(metaStoreType);
    set_use_fake_mgmtd_client(useFakeMgmtdClient);
    set_start_storage_server(startStorageServer);
    set_use_temp_path(useTempPath);
  }

  static std::string prettyPrintConfig(const testing::TestParamInfo<SystemSetupConfig> &info) {
    return fmt::format("{}replicas_{}nodes_client{}_metastore{}_fakemgmtd{}",
                       info.param.num_replicas(),
                       info.param.num_storage_nodes(),
                       static_cast<int>(info.param.client_impl_type()),
                       static_cast<int>(info.param.meta_store_type()),
                       info.param.use_fake_mgmtd_client());
  }
};

using NodeTargetMap = std::map<storage::NodeId, std::vector<storage::TargetId>>;

class UnitTestFabric {
 public:
  static NodeTargetMap buildNodeTargetMap(uint32_t numNodes, uint32_t numTargetsPerNode);

  /*
  Suppose numChains = 4, numReplicas = 3 and nodeTargets is a 2-d array:
  ----------------------------------------------
   node1 | target1001 | target1002 | target1003
   node2 | target2001 | target2002 | target2003
   node3 | target3001 | target3002 | target3003
   node4 | target4001 | target4002 | target4003
  ----------------------------------------------
  Then this function generates the following routing table:
  -----------------------------------------------
   chain1 | target1001 | target2001 | target3001
   chain2 | target4001 | target1002 | target2002
   chain3 | target3002 | target4002 | target1003
   chain4 | target2003 | target3003 | target4003
  -----------------------------------------------
  */
  static std::map<storage::ChainId, storage::client::SlimChainInfo> buildRepliaChainMap(uint32_t numChains,
                                                                                        uint32_t numReplicas,
                                                                                        NodeTargetMap nodeTargets);

  static std::shared_ptr<hf3fs::flat::RoutingInfo> createRoutingInfo(
      const std::map<storage::ChainId, storage::client::SlimChainInfo> &replicaChains,
      const std::map<storage::NodeId, net::Address> &nodeEndpoints);

  /* Set up and tear down */

 public:
  UnitTestFabric(const SystemSetupConfig &setupConfig)
      : setupConfig_(setupConfig),
        requestExe_(std::max(1U, std::thread::hardware_concurrency() / 2),
                    std::make_shared<folly::NamedThreadFactory>("UnitTestFabric")) {}

  bool setUpStorageSystem();

  void tearDownStorageSystem();

  std::unique_ptr<storage::StorageServer> createStorageServer(size_t nodeIndex);

  std::shared_ptr<hf3fs::flat::RoutingInfo> getRoutingInfo();

  bool updateRoutingInfo(std::function<void(hf3fs::flat::RoutingInfo &)> callback);

  auto copyRoutingInfo() { return std::make_shared<hf3fs::flat::RoutingInfo>(*rawRoutingInfo_); }

  void setTargetOffline(hf3fs::flat::RoutingInfo &routingInfo, uint32_t targetIndex, uint32_t chainIndex = 0);

  bool stopAndRemoveStorageServer(uint32_t serverIndex);

  bool stopAndRemoveStorageServer(storage::NodeId nodeId);

  /* Helper functions */

  storage::IOResult writeToChunk(storage::ChainId chainId,
                                 storage::ChunkId chunkId,
                                 std::span<uint8_t> chunkData,
                                 uint32_t offset = 0,
                                 uint32_t length = 0,
                                 const storage::client::WriteOptions &options = storage::client::WriteOptions());

  bool writeToChunks(storage::ChainId chainId,
                     storage::ChunkId chunkBegin,
                     storage::ChunkId chunkEnd,
                     std::span<uint8_t> chunkData,
                     uint32_t offset = 0,
                     uint32_t length = 0,
                     const storage::client::WriteOptions &options = storage::client::WriteOptions(),
                     std::vector<storage::IOResult> *results = nullptr);

  storage::IOResult readFromChunk(storage::ChainId chainId,
                                  storage::ChunkId chunkId,
                                  std::span<uint8_t> chunkData,
                                  uint32_t offset = 0,
                                  uint32_t length = 0,
                                  const storage::client::ReadOptions &options = storage::client::ReadOptions());

  bool readFromChunks(storage::ChainId chainId,
                      storage::ChunkId chunkBegin,
                      storage::ChunkId chunkEnd,
                      std::vector<std::vector<uint8_t>> &chunkData,
                      uint32_t offset = 0,
                      uint32_t length = 0,
                      const storage::client::ReadOptions &options = storage::client::ReadOptions(),
                      std::vector<storage::IOResult> *results = nullptr);

 protected:
  // the id of the only table
  static flat::ChainTableId kTableId() {
    static const auto v = flat::ChainTableId(1);
    return v;
  }

  // the initial routing table version
  static const flat::RoutingInfoVersion kRoutingVersion() {
    static const auto v = flat::RoutingInfoVersion(10);
    return v;
  }

  // the initial chain version
  static const storage::ChainVer kInitChainVer() {
    static const auto v = storage::ChainVer(1);
    return v;
  }

  constexpr static auto kClusterId = "test";
  char const *const kTempDataFolderPrefix = "temp_storage_data";

  ClientId clientId_ = ClientId::random();
  storage::ChainId firstChainId_;
  std::vector<storage::ChainId> chainIds_;
  NodeTargetMap nodeTargets_;
  std::vector<storage::NodeId> storageNodeIds_;
  std::map<storage::ChainId, storage::client::SlimChainInfo> replicaChains_;
  std::shared_ptr<hf3fs::flat::RoutingInfo> rawRoutingInfo_;

  std::vector<std::vector<folly::test::TemporaryDirectory>> tmpDataPaths_;
  std::vector<storage::StorageTargets::Config> targetsConfigs_;
  std::vector<storage::StorageServer::Config> serverConfigs_;
  std::vector<std::unique_ptr<storage::StorageServer>> storageServers_;
  MgmtdServerWithConfig mgmtdServer_{kClusterId, flat::NodeId(10000)};

  SystemSetupConfig setupConfig_;
  storage::client::StorageClient::Config clientConfig_;
  net::Client::Config netClientConfig_;
  net::Client client_{netClientConfig_};
  hf3fs::client::MgmtdClientForClient::Config mgmtdClientConfig_;
  std::unique_ptr<hf3fs::client::ICommonMgmtdClient> mgmtdForClient_;
  std::shared_ptr<storage::client::StorageClient> storageClient_;
  folly::CPUThreadPoolExecutor requestExe_;
};

}  // namespace hf3fs::test
