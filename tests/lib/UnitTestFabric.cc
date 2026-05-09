#include "UnitTestFabric.h"

#include <boost/filesystem/operations.hpp>
#include <folly/experimental/coro/Collect.h>

#include "client/mgmtd/MgmtdClientForClient.h"
#include "common/utils/StringUtils.h"
#include "common/utils/SysResource.h"
#include "tests/lib/Helper.h"

namespace hf3fs::test {

NodeTargetMap UnitTestFabric::buildNodeTargetMap(uint32_t numNodes, uint32_t numTargetsPerNode) {
  NodeTargetMap nodeTargets;

  for (storage::NodeId nodeId(1); nodeId <= numNodes; nodeId += 1) {
    std::vector<storage::TargetId> localTargets;

    storage::TargetId targetId(nodeId * 100 + 1);

    while (localTargets.size() < numTargetsPerNode) {
      localTargets.push_back(targetId);
      targetId += 1;
    }

    nodeTargets.emplace(nodeId, localTargets);
  }

  return nodeTargets;
}

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
This routing table has a special property: every node has a target working as
head, middle and tail target.
*/
std::map<storage::ChainId, storage::client::SlimChainInfo>
UnitTestFabric::buildRepliaChainMap(uint32_t numChains, uint32_t numReplicas, NodeTargetMap nodeTargets) {
  std::vector<storage::NodeId> serviceNodeIds;
  serviceNodeIds.reserve(nodeTargets.size());

  for (const auto &[nodeId, localTargets] : nodeTargets) {
    serviceNodeIds.push_back(nodeId);
  }

  std::map<storage::ChainId, storage::client::SlimChainInfo> replicaChains;
  uint32_t nodeIndex = 0;

  for (uint32_t chainId = 1; chainId <= numChains; chainId++) {
    auto tableChainId = (kTableId() << 16) + chainId;
    storage::client::SlimChainInfo replicaChain = {hf3fs::flat::ChainId(tableChainId),
                                                   kInitChainVer(),
                                                   kRoutingVersion(),
                                                   numReplicas,
                                                   {}};

    for (; replicaChain.servingTargets.size() < numReplicas; nodeIndex++) {
      auto nodeId = serviceNodeIds[nodeIndex % serviceNodeIds.size()];
      if (nodeTargets[nodeId].empty()) continue;
      auto targetId = nodeTargets[nodeId].front();
      replicaChain.servingTargets.push_back({targetId, nodeId});
      nodeTargets[nodeId].erase(nodeTargets[nodeId].begin());
    }

    replicaChains.emplace(tableChainId, replicaChain);
  }

  return replicaChains;
}

std::shared_ptr<hf3fs::flat::RoutingInfo> UnitTestFabric::createRoutingInfo(
    const std::map<storage::ChainId, storage::client::SlimChainInfo> &replicaChains,
    const std::map<storage::NodeId, net::Address> &nodeEndpoints) {
  auto routingInfo = std::make_shared<hf3fs::flat::RoutingInfo>();
  routingInfo->routingInfoVersion = kRoutingVersion();

  hf3fs::flat::ChainTable chainTable;
  chainTable.chainTableId = kTableId();

  for (const auto &[chainId, replicaChain] : replicaChains) {
    hf3fs::flat::ChainInfo chainInfo;
    chainInfo.chainId = hf3fs::flat::ChainId(chainId);
    chainInfo.chainVersion = replicaChain.version;

    // for (const auto &target : replicaChain.servingTargets) {
    for (auto &target : replicaChain.servingTargets) {
      auto publicState = hf3fs::flat::PublicTargetState::SERVING;
      auto localState = hf3fs::flat::LocalTargetState::UPTODATE;  // build chain target info
      // build chain target info
      hf3fs::flat::ChainTargetInfo chainTargetInfo;
      chainTargetInfo.targetId = hf3fs::flat::TargetId(target.targetId);
      chainTargetInfo.publicState = publicState;
      chainInfo.targets.push_back(chainTargetInfo);

      // build and add target info
      hf3fs::flat::TargetInfo targetInfo;
      targetInfo.targetId = hf3fs::flat::TargetId(target.targetId);
      targetInfo.chainId = hf3fs::flat::ChainId(chainId);
      targetInfo.nodeId = hf3fs::flat::NodeId(target.nodeId);
      targetInfo.publicState = publicState;
      targetInfo.localState = localState;
      routingInfo->targets.emplace(targetInfo.targetId, targetInfo);
    }

    // add chain info
    chainTable.chains.push_back(chainInfo.chainId);
    routingInfo->chains[chainInfo.chainId] = chainInfo;
  }

  // add chain table
  routingInfo->chainTables[kTableId()][hf3fs::flat::ChainTableVersion(1)] = chainTable;

  for (const auto &[nodeId, endpoint] : nodeEndpoints) {
    hf3fs::flat::ServiceGroupInfo serviceGroupInfo;
    serviceGroupInfo.endpoints = {endpoint};

    hf3fs::flat::NodeInfo nodeInfo;
    nodeInfo.app.nodeId = hf3fs::flat::NodeId(nodeId);
    nodeInfo.type = hf3fs::flat::NodeType::STORAGE;
    nodeInfo.app.serviceGroups.push_back(serviceGroupInfo);
    routingInfo->nodes.emplace(nodeInfo.app.nodeId, nodeInfo);
  }

  return routingInfo;
}

std::unique_ptr<storage::StorageServer> UnitTestFabric::createStorageServer(size_t nodeIndex) {
  const auto storageEndpoints = setupConfig_.storage_endpoints();
  const auto serverConfigPath = setupConfig_.server_config();
  const auto listenPort = setupConfig_.listen_port();
  const auto metaStoreType = setupConfig_.meta_store_type();

  XLOGF(INFO, "Creating storage server #{}...", nodeIndex);

  auto nodeId = storageNodeIds_[nodeIndex];
  storage::StorageServer::Config &serverConfig = serverConfigs_[nodeIndex];
  const auto &mgmtdAddressList = mgmtdServer_.collectAddressList("Mgmtd");

  XLOGF(INFO, "Preparing storage config #{} {}...", nodeIndex, nodeId);

  if (!serverConfigPath.empty()) {
    auto configRes = serverConfig.atomicallyUpdate(serverConfigPath, false /*isHotUpdate*/);
    if (!configRes) {
      XLOGF(ERR, "Cannot load server config from {}, error: {}", serverConfigPath, configRes.error());
      return nullptr;
    }
  } else {
    serverConfig.base().thread_pool().set_num_io_threads(16);
    serverConfig.base().thread_pool().set_num_proc_threads(16);
    serverConfig.storage().write_worker().set_num_threads(16);
    serverConfig.storage().set_post_buffer_per_bytes(64_KB);
    serverConfig.storage().set_max_num_results_per_query(3);
    serverConfig.storage().set_rdma_transmission_req_timeout(100_ms);

    serverConfig.aio_read_worker().set_num_threads(16);
    serverConfig.aio_read_worker().set_ioengine(storage::AioReadWorker::IoEngine::random);
    serverConfig.buffer_pool().set_rdmabuf_count(256);
    serverConfig.buffer_pool().set_big_rdmabuf_count(4);

    serverConfig.mgmtd().set_enable_auto_refresh(true);
    serverConfig.mgmtd().set_enable_auto_heartbeat(true);
    serverConfig.mgmtd().set_auto_refresh_interval(mgmtdServer_.config.service().check_status_interval());
    serverConfig.mgmtd().set_auto_heartbeat_interval(mgmtdServer_.config.service().check_status_interval());

    // set server side retry timeout
    serverConfig.reliable_forwarding().set_retry_first_wait(200_ms);
    serverConfig.reliable_forwarding().set_retry_max_wait(500_ms);
    serverConfig.reliable_forwarding().set_retry_total_time(300_s);
    serverConfig.reliable_forwarding().set_max_inline_forward_bytes(8_KB);

    // enable trace log
    serverConfig.storage().event_trace_log().set_enabled(true);
    serverConfig.storage().event_trace_log().set_dump_interval(5_s);
    serverConfig.storage().event_trace_log().set_max_num_writers(8);
  }

  auto chunkSizeList = setupConfig_.getChunkSizeList();
  serverConfig.base().groups(0).listener().set_listen_port(listenPort);
  serverConfig.base().groups(1).listener().set_listen_port(0);
  serverConfig.targets() = targetsConfigs_[nodeIndex];
  serverConfig.targets().storage_target().set_force_persist(false);
  serverConfig.targets().storage_target().kv_store().set_type(metaStoreType);
  serverConfig.targets().storage_target().file_store().set_preopen_chunk_size_list(
      std::set(chunkSizeList.begin(), chunkSizeList.end()));
  serverConfig.mgmtd().set_mgmtd_server_addresses(to_named(mgmtdAddressList));
  serverConfig.coroutines_pool_read().set_threads_num(32);
  serverConfig.coroutines_pool_read().set_coroutines_num(4096);
  serverConfig.coroutines_pool_update().set_threads_num(32);
  serverConfig.coroutines_pool_update().set_coroutines_num(4096);
  serverConfig.set_speed_up_quit(false);

  XLOGF(INFO, "Allocating storage server #{} {}...", nodeIndex, nodeId);
  auto storageMemory =
      std::make_unique<std::aligned_storage_t<sizeof(storage::StorageServer), alignof(storage::StorageServer)>>();
  XLOGF(INFO, "Consturcting storage server #{} {}...", nodeIndex, nodeId);
  std::unique_ptr<storage::StorageServer> storageServer{new (storageMemory.release())
                                                            storage::StorageServer(serverConfig)};

  XLOGF(INFO, "Setting up storage server #{} {}...", nodeIndex, nodeId);
  auto result = storageServer->setup();
  if (!result) {
    XLOGF(ERR, "Cannot setup storage server: {}", result.error());
    return nullptr;
  }

  auto nodeEndpoint = storageServer->groups().front()->addressList().front();
  XLOGF(WARN, "Storage server address of {}: {}", nodeId, nodeEndpoint);

  hf3fs::flat::ServiceGroupInfo serviceGroupInfo;
  serviceGroupInfo.endpoints = {nodeEndpoint};

  flat::AppInfo appInfo;
  appInfo.nodeId = nodeId;
  appInfo.clusterId = kClusterId;
  appInfo.serviceGroups.push_back(serviceGroupInfo);

  XLOGF(INFO, "Starting storage server #{} {}...", nodeIndex, nodeId);
  result = storageServer->start(appInfo);
  if (!result) {
    XLOGF(ERR, "Cannot start storage server: {}", result.error());
    return nullptr;
  }

  XLOGF(INFO, "Started storage server #{} {}...", nodeIndex, nodeId);
  return storageServer;
}

bool UnitTestFabric::setUpStorageSystem() {
  XLOGF(INFO, "setUpStorageSystem started!");

  const auto numChains = setupConfig_.num_chains();
  const auto numReplicas = setupConfig_.num_replicas();
  const auto numStorageNodes = setupConfig_.num_storage_nodes();
  const auto dataPaths = setupConfig_.data_paths();
  const auto numDataPaths = setupConfig_.data_paths().size();
  const auto clientConfigPath = setupConfig_.client_config();
  const auto serverConfigPath = setupConfig_.server_config();
  const auto clientImplType = setupConfig_.client_impl_type();
  const auto storageEndpoints = setupConfig_.storage_endpoints();
  const auto serviceLevel = setupConfig_.service_level();

  // validate options

  if (numStorageNodes < numReplicas) {
    XLOGF(ERR, "Invalid arguments: numStorageNodes {} < numReplicas {}", numStorageNodes, numReplicas);
    return false;
  }

  size_t totalNumTargets = numChains * numReplicas;
  size_t totalNumDataPaths = numStorageNodes * numDataPaths;

  if (0 != totalNumTargets % totalNumDataPaths) {
    XLOGF(ERR,
          "Invalid arguments: numChains {} * numReplicas {} % numStorageNodes {} * numDataPaths {} != 0",
          numChains,
          numReplicas,
          numStorageNodes,
          numDataPaths);
    return false;
  }

  uint32_t numTargetsPerNode = totalNumTargets / numStorageNodes;
  uint32_t numTargetsPerDataPath = numTargetsPerNode / numDataPaths;

  // build routing info

  XLOGF(INFO, "Building replication chains...");
  nodeTargets_ = buildNodeTargetMap(numStorageNodes, numTargetsPerNode);
  replicaChains_ = buildRepliaChainMap(numChains, numReplicas, nodeTargets_);

  for (const auto &[chainId, _] : replicaChains_) {
    chainIds_.push_back(chainId);
  }

  firstChainId_ = chainIds_.front();
  for (const auto &[nodeId, _] : nodeTargets_) storageNodeIds_.push_back(nodeId);

  // initialize storage targets

  tmpDataPaths_.resize(numStorageNodes);
  targetsConfigs_.resize(numStorageNodes);
  serverConfigs_.resize(numStorageNodes);

  for (uint32_t nodeIndex = 0; nodeIndex < numStorageNodes; nodeIndex++) {
    // create a temp subfolder under each data path for the storage node
    std::vector<Path> targetPaths;
    if (setupConfig_.use_temp_path()) {
      for (const auto &dataPath : dataPaths) {
        tmpDataPaths_[nodeIndex].emplace_back(
            fmt::format("{}_node{:02d}" /*namePrefix*/, kTempDataFolderPrefix, nodeIndex),
            dataPath);
        targetPaths.push_back(tmpDataPaths_[nodeIndex].back().path().string());
      }
    } else {
      // convert the temp paths to Path objects accepted by storage target creater
      for (const auto &dataPath : dataPaths) {
        auto tmpPath = dataPath / fmt::format("{}_node{:02d}", kTempDataFolderPrefix, nodeIndex);
        boost::system::error_code ec;
        boost::filesystem::create_directory(tmpPath, ec);
        if (ec.failed()) {
          XLOGF(ERR, "create target path {} failed, error: {}", tmpPath, ec.message());
          return false;
        }
        targetPaths.push_back(tmpPath);
      }
    }

    targetsConfigs_[nodeIndex].set_target_num_per_path(numTargetsPerDataPath);
    targetsConfigs_[nodeIndex].set_target_paths(targetPaths);
    targetsConfigs_[nodeIndex].storage_target().kv_store().set_type(setupConfig_.meta_store_type());
    targetsConfigs_[nodeIndex].set_allow_disk_without_uuid(true);

    std::vector<flat::TargetId::UnderlyingType> numericTargetIds;
    for (auto targetId : nodeTargets_[storageNodeIds_[nodeIndex]]) numericTargetIds.push_back(targetId.toUnderType());

    storage::StorageTargets::CreateConfig createConfig;
    createConfig.set_chunk_size_list(setupConfig_.getChunkSizeList());
    createConfig.set_physical_file_count(8);
    createConfig.set_allow_disk_without_uuid(true);
    createConfig.set_target_ids(numericTargetIds);
    createConfig.set_allow_existing_targets(!setupConfig_.use_temp_path());
    createConfig.set_only_chunk_engine(true);

    XLOGF(INFO, "Creating storage targets on node #{}", nodeIndex);
    storage::AtomicallyTargetMap targetMap;
    storage::StorageTargets targets(targetsConfigs_[nodeIndex], targetMap);
    auto result = targets.create(createConfig);

    if (!result) {
      XLOGF(ERR, "Cannot create storage target: {}", result.error());
      return false;
    }
  }

  // initialize storage servers

  XLOGF(INFO, "Starting mgmtd server...");

  auto kvEngine = std::make_shared<kv::MemKVEngine>();

  mgmtdServer_.config.service().set_check_status_interval(300_ms);
  mgmtdServer_.config.service().set_heartbeat_fail_interval(1_s);
  mgmtdServer_.config.service().set_allow_heartbeat_from_unregistered(true);

  if (!mgmtdServer_.start(kvEngine)) {
    XLOGF(ERR, "Failed to start mgmtd server");
    return false;
  }

  auto mgmtdAddressList = mgmtdServer_.collectAddressList("Mgmtd");

  if (mgmtdAddressList.empty()) {
    XLOGF(ERR, "Empty list of mgmtd server address");
    return false;
  }

  for (const auto &address : mgmtdAddressList) {
    XLOGF(INFO, "Mgmtd server address: {}", address);
  }

  std::map<storage::NodeId, net::Address> nodeEndpoints;

  if (!setupConfig_.start_storage_server()) {
    for (auto &[key, value] : storageEndpoints) {
      nodeEndpoints[(storage::NodeId)std::stoul(key)] = value;
    }
  } else {
    for (uint32_t nodeIndex = 0; nodeIndex < numStorageNodes; nodeIndex++) {
      auto nodeId = storageNodeIds_[nodeIndex];

      if (!storageEndpoints.empty()) {
        auto iter = storageEndpoints.find(std::to_string(nodeId));

        if (iter == storageEndpoints.end()) {
          XLOGF(ERR, "Cannot find the endpoint of {}", nodeId);
          return false;
        }

        if (iter->second.ip != 0) {
          nodeEndpoints[nodeId] = iter->second;
          continue;
        }
      }

      auto storageServer = createStorageServer(nodeIndex);
      if (storageServer == nullptr) {
        return false;
      }
      nodeEndpoints[nodeId] = storageServer->groups().front()->addressList().front();
      storageServers_.push_back(std::move(storageServer));
    }
  }

  XLOGF(INFO, "Creating routing info...");
  rawRoutingInfo_ = createRoutingInfo(replicaChains_, nodeEndpoints);

  XLOGF(INFO, "Starting {} mgmtd client...", setupConfig_.use_fake_mgmtd_client() ? "fake" : "real");
  if (setupConfig_.use_fake_mgmtd_client()) {
    for (auto &server : storageServers_) {
      auto mgmtdClient = std::make_unique<FakeMgmtdClient>(rawRoutingInfo_);
      std::unique_ptr<hf3fs::client::IMgmtdClientForServer> client = std::move(mgmtdClient);
      RoutingStoreHelper::setMgmtdClient(*server, std::move(client));
    }

    mgmtdForClient_.reset((new FakeMgmtdClient(rawRoutingInfo_))->asCommon());
  } else {
    mgmtdClientConfig_.set_mgmtd_server_addresses(to_named(mgmtdAddressList));
    mgmtdClientConfig_.set_enable_auto_refresh(true);
    mgmtdClientConfig_.set_enable_auto_heartbeat(false);
    mgmtdClientConfig_.set_auto_refresh_interval(mgmtdServer_.config.service().check_status_interval());
    mgmtdClientConfig_.set_auto_heartbeat_interval(mgmtdServer_.config.service().check_status_interval());
    mgmtdClientConfig_.set_accept_incomplete_routing_info_during_mgmtd_bootstrapping(true);

    if (!client_.start()) {
      XLOGF(ERR, "Failed to start net client for mgmtd client");
      return false;
    }

    auto stubFactory = std::make_unique<hf3fs::stubs::RealStubFactory<hf3fs::mgmtd::MgmtdServiceStub>>(
        stubs::ClientContextCreator{[this](net::Address addr) { return client_.serdeCtx(addr); }});
    auto mgmtdClient =
        std::make_unique<hf3fs::client::MgmtdClientForClient>(kClusterId, std::move(stubFactory), mgmtdClientConfig_);

    auto physicalHostnameRes = SysResource::hostname(/*physicalMachineName=*/true);
    if (!physicalHostnameRes) {
      XLOGF(ERR, "getHostname(true) failed: {}", physicalHostnameRes.error());
      return false;
    }

    auto containerHostnameRes = SysResource::hostname(/*physicalMachineName=*/false);
    if (!containerHostnameRes) {
      XLOGF(ERR, "getHostname(false) failed: {}", containerHostnameRes.error());
      return false;
    }

    mgmtdClient->setClientSessionPayload({clientId_.uuid.toHexString(),
                                          flat::NodeType::CLIENT,
                                          flat::ClientSessionData::create(
                                              /*universalId=*/*physicalHostnameRes,
                                              /*description=*/fmt::format("UnitTestFabric: {}", *containerHostnameRes),
                                              /*serviceGroups=*/std::vector<flat::ServiceGroupInfo>{},
                                              flat::ReleaseVersion::fromVersionInfo()),
                                          flat::UserInfo{}});
    folly::coro::blockingWait(mgmtdClient->start(&client_.tpg().bgThreadPool().randomPick()));
    mgmtdForClient_ = std::move(mgmtdClient);
  }

  XLOGF(INFO, "Updating routing info {}...", rawRoutingInfo_->routingInfoVersion);
  updateRoutingInfo([&](auto &routingInfo) { routingInfo = *rawRoutingInfo_; });

  // create storage client

  if (!clientConfigPath.empty()) {
    auto configRes = clientConfig_.atomicallyUpdate(clientConfigPath, false /*isHotUpdate*/);
    if (!configRes) {
      XLOGF(ERR, "Cannot load client config from {}, error: {}", clientConfigPath, configRes.error());
      return false;
    }
  } else {
    clientConfig_.retry().set_init_wait_time(200_ms);
    clientConfig_.retry().set_max_wait_time(500_ms);
    clientConfig_.retry().set_max_retry_time(120_s);
    clientConfig_.net_client().thread_pool().set_num_io_threads(32);
    clientConfig_.net_client().thread_pool().set_num_proc_threads(32);
    clientConfig_.net_client().set_default_timeout(1_s);
    clientConfig_.net_client().io_worker().ibsocket().set_max_rdma_wr(1024U);
    clientConfig_.net_client().io_worker().ibsocket().set_max_rdma_wr_per_post(128U);
    clientConfig_.net_client().io_worker().transport_pool().set_max_connections(256);
    clientConfig_.net_client().set_enable_rdma_control(true);

    clientConfig_.set_create_net_client_for_updates(true);
    clientConfig_.net_client_for_updates() = clientConfig_.net_client();

    clientConfig_.set_max_inline_read_bytes(8_KB);
    clientConfig_.set_max_inline_write_bytes(8_KB);
    clientConfig_.set_max_read_io_bytes(64_KB);
  }

  clientConfig_.set_implementation_type(clientImplType);
  clientConfig_.net_client().io_worker().ibsocket().set_sl(serviceLevel);

  XLOGF(INFO, "Creating storage client...");
  storageClient_ = storage::client::StorageClient::create(clientId_, clientConfig_, *mgmtdForClient_);

  XLOGF(INFO, "setUpStorageSystem finished!");
  return true;
}

void UnitTestFabric::tearDownStorageSystem() {
  XLOGF(INFO, "tearDownStorageSystem started!");

  XLOGF(INFO, "Stopping storage client");
  if (storageClient_) {
    storageClient_->stop();
    storageClient_.reset();
  }

  XLOGF(INFO, "Stopping mgmtd client");
  if (mgmtdForClient_) {
    folly::coro::blockingWait(mgmtdForClient_->stop());
    mgmtdForClient_.reset();
  }

  XLOGF(INFO, "Stopping {} storage servers", storageServers_.size());
  for (auto &storageServer : storageServers_) {
    if (storageServer) storageServer->stopAndJoin();
  }

  storageServers_.clear();

  XLOGF(INFO, "Stopping mgmtd server");
  mgmtdServer_.stop();

  XLOGF(INFO, "tearDownStorageSystem finished!");
}

std::shared_ptr<hf3fs::flat::RoutingInfo> UnitTestFabric::getRoutingInfo() {
  // get routing info

  for (size_t retry = 0; retry < 15; retry++) {
    auto routingInfo = mgmtdForClient_->getRoutingInfo();

    if (routingInfo == nullptr || routingInfo->raw()->chains.empty()) {
      XLOGF(WARN, "Empty routing info, #{} retry...", retry + 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    } else {
      for (const auto &[tableId, tableVersions] : routingInfo->raw()->chainTables) {
        if (tableId == kTableId()) {
          if (tableVersions.empty()) {
            XLOGF(WARN, "No version found for chain table with id {}", tableId);
            return nullptr;
          }

          const auto iter = --tableVersions.cend();
          const auto &latestTable = iter->second;

          XLOGF(DBG1, "Latest routing info: {:?}", *routingInfo->raw());
          XLOGF(INFO,
                "Summary of routing info {}: bootstrapping? {}, {} nodes, {} chain tables, {} chains, {} targets",
                routingInfo->raw()->routingInfoVersion,
                routingInfo->raw()->bootstrapping,
                routingInfo->raw()->nodes.size(),
                routingInfo->raw()->chainTables.size(),
                routingInfo->raw()->chains.size(),
                routingInfo->raw()->targets.size());

          chainIds_ = latestTable.chains;
          rawRoutingInfo_ = routingInfo->raw();
          return rawRoutingInfo_;
        }
      }
    }
  }

  XLOGF(ERR, "Failed to get chain table with id {}", kTableId());
  return nullptr;
}

bool UnitTestFabric::updateRoutingInfo(std::function<void(hf3fs::flat::RoutingInfo &)> callback) {
  if (setupConfig_.use_fake_mgmtd_client()) {
    auto newRoutingInfo = copyRoutingInfo();
    callback(*newRoutingInfo);
    ++newRoutingInfo->routingInfoVersion.toUnderType();

    for (auto &server : storageServers_) {
      auto client = RoutingStoreHelper::getMgmtdClient(*server);
      auto fakeClient = dynamic_cast<FakeMgmtdClient *>(client.get());
      fakeClient->setRoutingInfo(newRoutingInfo);
    }

    auto fakeClient = dynamic_cast<FakeMgmtdClient *>(mgmtdForClient_.get());
    fakeClient->setRoutingInfo(newRoutingInfo);
  } else {
    hf3fs::mgmtd::testing::MgmtdTestHelper mgmtdHelper(*mgmtdServer_.server);

    auto updateRes = folly::coro::blockingWait(mgmtdHelper.setRoutingInfo(std::move(callback)));

    if (!updateRes) {
      XLOGF(ERR, "Failed to update routing info, error: {}", updateRes.error());
      return false;
    }
  }

  for (auto &server : storageServers_) {
    RoutingStoreHelper::refreshRoutingInfo(*server);
  }

  auto refreshRes = folly::coro::blockingWait(mgmtdForClient_->refreshRoutingInfo(/*force=*/true));

  if (!refreshRes) {
    XLOGF(ERR, "Failed to refresh routing info, error: {}", refreshRes.error());
    return false;
  }

  XLOGF(INFO, "Updated routing info, useFakeMgmtdClient: {}", setupConfig_.use_fake_mgmtd_client());
  rawRoutingInfo_ = mgmtdForClient_->getRoutingInfo()->raw();
  return true;
}

void UnitTestFabric::setTargetOffline(hf3fs::flat::RoutingInfo &routingInfo,
                                      uint32_t targetIndex,
                                      uint32_t chainIndex) {
  auto &chainTable = *routingInfo.getChainTable(kTableId());
  auto &chainInfo = routingInfo.chains[chainTable.chains[chainIndex]];

  // set the target to offline state
  auto targetState =
      chainInfo.targets.size() == 1 ? hf3fs::flat::PublicTargetState::LASTSRV : hf3fs::flat::PublicTargetState::OFFLINE;
  auto offlineTarget = chainInfo.targets[targetIndex];
  offlineTarget.publicState = targetState;
  auto &targetInfo = routingInfo.targets[offlineTarget.targetId];
  targetInfo.publicState = targetState;
  targetInfo.localState = flat::LocalTargetState::OFFLINE;

  // move the target to the end of chain
  chainInfo.targets.erase(chainInfo.targets.begin() + targetIndex);
  chainInfo.targets.push_back(offlineTarget);

  // increment chain/routing versions
  chainInfo.chainVersion++;
  routingInfo.routingInfoVersion++;

  XLOGF(INFO,
        "Set #{} target {} on {}@{} to {} state",
        targetIndex + 1,
        offlineTarget.targetId,
        chainInfo.chainId,
        chainInfo.chainVersion,
        toStringView(targetState));
}

bool UnitTestFabric::stopAndRemoveStorageServer(uint32_t serverIndex) {
  if (serverIndex >= storageServers_.size()) return false;
  storageServers_[serverIndex]->stopAndJoin();
  storageServers_.erase(storageServers_.begin() + serverIndex);
  XLOGF(INFO, "Stopped storage server #{}", serverIndex);
  return true;
}

bool UnitTestFabric::stopAndRemoveStorageServer(storage::NodeId nodeId) {
  for (auto iter = storageServers_.begin(); iter != storageServers_.end(); iter++) {
    if ((*iter)->appInfo().nodeId == nodeId) {
      (*iter)->stopAndJoin();
      storageServers_.erase(iter);
      XLOGF(INFO, "Stopped storage server {}", nodeId);
      return true;
    }
  }

  XLOGF(ERR, "Cannot find storage server with {}", nodeId);
  return false;
}

/* Helper functions */

storage::IOResult UnitTestFabric::writeToChunk(storage::ChainId chainId,
                                               storage::ChunkId chunkId,
                                               std::span<uint8_t> chunkData,
                                               uint32_t offset,
                                               uint32_t length,
                                               const storage::client::WriteOptions &options) {
  // register a block of memory

  if (length == 0) {
    length = chunkData.size();
  }

  auto regRes = storageClient_->registerIOBuffer(&chunkData[0], chunkData.size());

  if (!regRes) {
    return storage::IOResult{regRes.error().code()};
  }

  // create write IO

  auto ioBuffer = std::move(*regRes);
  auto writeIO = storageClient_->createWriteIO(chainId,
                                               chunkId,
                                               offset,
                                               length,
                                               setupConfig_.chunk_size() /*chunkSize*/,
                                               &chunkData[0],
                                               &ioBuffer);

  folly::coro::blockingWait(storageClient_->write(writeIO, flat::UserInfo(), options));

  return writeIO.result;
}

bool UnitTestFabric::writeToChunks(storage::ChainId chainId,
                                   storage::ChunkId chunkBegin,
                                   storage::ChunkId chunkEnd,
                                   std::span<uint8_t> chunkData,
                                   uint32_t offset,
                                   uint32_t length,
                                   const storage::client::WriteOptions &options,
                                   std::vector<storage::IOResult> *results) {
  // register a block of memory

  if (length == 0) {
    length = chunkData.size();
  }

  auto regRes = storageClient_->registerIOBuffer(&chunkData[0], chunkData.size());

  if (!regRes) {
    return false;
  }

  auto ioBuffer = std::move(*regRes);

  // create write IOs

  std::vector<storage::client::WriteIO> writeIOs;
  storage::ChunkId chunkId = chunkBegin;

  for (uint32_t chunkIndex = 0; chunkId < chunkEnd; chunkId = storage::ChunkId(chunkBegin, ++chunkIndex)) {
    auto writeIO = storageClient_->createWriteIO(chainId,
                                                 chunkId,
                                                 offset /*offset*/,
                                                 length /*length*/,
                                                 setupConfig_.chunk_size() /*chunkSize*/,
                                                 &chunkData[0],
                                                 &ioBuffer);
    writeIOs.push_back(std::move(writeIO));
  }

  flat::UserInfo dummyUserInfo{};
  std::vector<folly::SemiFuture<folly::Expected<folly::Unit, hf3fs::Status>>> ioTasks;

  for (auto &writeIO : writeIOs) {
    auto task = storageClient_->write(writeIO, dummyUserInfo, options).scheduleOn(&requestExe_).start();
    ioTasks.push_back(std::move(task));
  }

  folly::coro::blockingWait(folly::coro::collectAllRange(std::move(ioTasks)));

  for (size_t writeIndex = 0; writeIndex < writeIOs.size(); writeIndex++) {
    const auto &writeIO = writeIOs[writeIndex];

    if (results) results->push_back(writeIO.result);

    if (!writeIO.result.lengthInfo) {
      XLOGF(ERR,
            "Write IO #{}/{} to chunk {} on {}: error code {}",
            writeIndex + 1,
            writeIOs.size(),
            writeIO.chunkId,
            writeIO.routingTarget.chainId,
            writeIO.result.lengthInfo.error());
      return false;
    }

    if (writeIO.length != writeIO.result.lengthInfo.value()) {
      XLOGF(ERR,
            "Write IO #{}/{} to chunk {} on {}: unexpected length {} != expected value {}",
            writeIndex + 1,
            writeIOs.size(),
            writeIO.chunkId,
            writeIO.routingTarget.chainId,
            writeIO.result.lengthInfo.value(),
            writeIO.length);
      return false;
    }

    if (writeIO.result.commitVer != 1) {
      XLOGF(DBG,
            "Write IO #{}/{} to chunk {} on {}: commit version {} != 1",
            writeIndex + 1,
            writeIOs.size(),
            writeIO.chunkId,
            writeIO.routingTarget.chainId,
            writeIO.result.commitVer);
    }
  }

  return true;
}

storage::IOResult UnitTestFabric::readFromChunk(storage::ChainId chainId,
                                                storage::ChunkId chunkId,
                                                std::span<uint8_t> chunkData,
                                                uint32_t offset,
                                                uint32_t length,
                                                const storage::client::ReadOptions &options) {
  // register a block of memory

  if (length == 0) {
    length = chunkData.size();
  }

  auto regRes = storageClient_->registerIOBuffer(&chunkData[0], chunkData.size());

  if (!regRes) {
    return storage::IOResult{regRes.error().code()};
  }

  // create read IO

  auto ioBuffer = std::move(*regRes);
  auto readIO = storageClient_->createReadIO(chainId, chunkId, offset, length, &chunkData[0], &ioBuffer);

  folly::coro::blockingWait(storageClient_->read(readIO, flat::UserInfo(), options));

  return readIO.result;
}

bool UnitTestFabric::readFromChunks(storage::ChainId chainId,
                                    storage::ChunkId chunkBegin,
                                    storage::ChunkId chunkEnd,
                                    std::vector<std::vector<uint8_t>> &chunkData,
                                    uint32_t offset,
                                    uint32_t length,
                                    const storage::client::ReadOptions &options,
                                    std::vector<storage::IOResult> *results) {
  // register a block of memory
  std::vector<storage::client::IOBuffer> ioBuffers;
  storage::ChunkId chunkId = chunkBegin;

  for (uint32_t chunkIndex = 0; chunkId < chunkEnd; chunkId = storage::ChunkId(chunkBegin, ++chunkIndex)) {
    chunkData.resize(chunkIndex + 1);
    chunkData[chunkIndex].resize(length);

    auto regRes = storageClient_->registerIOBuffer(&chunkData[chunkIndex][0], chunkData[chunkIndex].size());

    if (!regRes) {
      return false;
    }

    ioBuffers.push_back(std::move(*regRes));
  }

  // create read IOs

  std::vector<storage::client::ReadIO> readIOs;
  chunkId = chunkBegin;

  for (uint32_t chunkIndex = 0; chunkId < chunkEnd; chunkId = storage::ChunkId(chunkBegin, ++chunkIndex)) {
    auto readIO = storageClient_->createReadIO(chainId,
                                               chunkId,
                                               offset /*offset*/,
                                               length /*length*/,
                                               &chunkData[chunkIndex][0],
                                               &ioBuffers[chunkIndex]);
    readIOs.push_back(std::move(readIO));
  }

  folly::coro::blockingWait(storageClient_->batchRead(readIOs, flat::UserInfo(), options));

  for (size_t readIndex = 0; readIndex < readIOs.size(); readIndex++) {
    const auto &readIO = readIOs[readIndex];

    if (results) results->push_back(readIO.result);

    if (!readIO.result.lengthInfo) {
      XLOGF(ERR,
            "Read IO #{}/{} from chunk {} on {}: error code {}",
            readIndex + 1,
            readIOs.size(),
            readIO.chunkId,
            readIO.routingTarget.chainId,
            readIO.result.lengthInfo.error());
      return false;
    }

    if (readIO.length != readIO.result.lengthInfo.value()) {
      XLOGF(DBG,
            "Read IO #{}/{} from chunk {} on {}: unexpected length {} != expected value {}",
            readIndex + 1,
            readIOs.size(),
            readIO.chunkId,
            readIO.routingTarget.chainId,
            readIO.result.lengthInfo.value(),
            readIO.length);
    }
  }

  return true;
}

}  // namespace hf3fs::test
