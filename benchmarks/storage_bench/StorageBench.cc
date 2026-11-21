#include "StorageBench.h"

#include <folly/init/Init.h>

#include "common/monitor/Monitor.h"
#include "memory/common/OverrideCppNewDelete.h"

DEFINE_bool(benchmarkNetwork, false, "Run in network benchmark mode");
DEFINE_bool(benchmarkStorage, false, "Run in storage benchmark mode");
DEFINE_bool(ignoreIOError, false, "Ignore all IO errors");
DEFINE_bool(injectRandomServerError, false, "Inject random server errors");
DEFINE_bool(injectRandomClientError, false, "Inject random client errors");
DEFINE_bool(retryPermanentError, false, "Retry requests with permanent errors");
DEFINE_bool(verifyReadData, false, "Check if the read data are correct");
DEFINE_bool(verifyReadChecksum, false, "Verify the checksum of read IOs");
DEFINE_bool(verifyWriteChecksum, true, "Verify the checksum of write IOs");
DEFINE_bool(randomShuffleChunkIds, false, "Random shuffle generated chunk IDs");
DEFINE_bool(generateTestData, true, "Generate test data for read test");
DEFINE_bool(sparseChunkIds, false, "Generate sparse chunk IDs");
DEFINE_bool(truncateChunks, false, "Truncate chunks");
DEFINE_bool(cleanupChunks, false, "Clean up (remove) chunks after benchmark");
DEFINE_bool(cleanupChunksBeforeBench, false, "Clean up (remove) chunks before benchmark");
DEFINE_bool(serverMode, false, "Run in server mode");
DEFINE_bool(clientMode, false, "Run in client mode");
DEFINE_bool(clusterMode, false, "Run in cluster mode (get routing info from mgmtd)");
DEFINE_bool(printMetrics, false, "Enable printing metrics in logs");
DEFINE_bool(reportMetrics, false, "Enable reporting metrics to ClickHouse");
DEFINE_uint32(metaStoreType, 0, "Metadata store type (0=LevelDB, 1=RocksDB, 2=MemDB)");
DEFINE_uint32(chunkSizeKB, 512, "Chunk size (KB)");
DEFINE_uint32(chainTableId, 0, "Chain table id");
DEFINE_uint32(chainTableVersion, 0, "Chain table version");
DEFINE_string(chainIds, "", "List of chain ids");
DEFINE_string(storageNodeIds, "", "List of storage node ids");
DEFINE_uint32(numChains, 1, "Number of chains");
DEFINE_uint32(numReplicas, 1, "Number of replicas");
DEFINE_uint32(numStorageNodes, 1, "Number of storage nodes");
DEFINE_uint32(numChunks, 1, "Number of chunks");
DEFINE_uint32(readSize, 4096, "Read IO size");
DEFINE_uint32(writeSize, 131072, "Write IO size");
DEFINE_uint32(memoryAlignment, 1, "Alignment size of each IO buffer");
DEFINE_uint32(readOffAlignment, 0, "Alignment size of each read IO offset");
DEFINE_uint32(batchSize, 1, "Read/write batch size");
DEFINE_uint32(readBatchSize, 0, "Read batch size");
DEFINE_uint32(writeBatchSize, 0, "Write batch size");
DEFINE_uint32(removeBatchSize, 0, "Remove batch size");
DEFINE_uint32(numReadSecs, 0, "Read test duration");
DEFINE_uint32(numWriteSecs, 0, "Write test duration");
DEFINE_uint32(numCoroutines, 1, "Number of coroutines");
DEFINE_uint32(numTestThreads, 1, "Number of test threads");
DEFINE_uint32(randSeed, 0, "Random seed for chunk id generation");
DEFINE_uint32(chunkIdPrefix, 0xFFFF, "The most significant 2 bytes of chunk ids");
DEFINE_uint32(serviceLevel, 0, "Service level");
DEFINE_uint32(listenPort, 0, "Listen port");
DEFINE_uint32(clientTimeoutMS, 30000, "Client timeout (milliseconds)");
DEFINE_string(dataPaths, folly::fs::temp_directory_path().string(), "Comma or space separated list of paths");
DEFINE_string(clientConfig, "", "Path of client config");
DEFINE_string(serverConfig, "", "Path of server config");
DEFINE_string(statsFilePath, "./perfstats.csv", "Path of performance stats file");
DEFINE_string(ibvDevices, "mlx5_0,mlx5_1", "Comma or space separated list of ibv devices");
DEFINE_string(ibnetZones, "", "Comma or space separated list of IB network zones");
DEFINE_string(clusterId, "stage", "Cluster id used to connect to mgmtd");
DEFINE_string(mgmtdEndpoints,
              "",
              "Comma or space separated list of mgmtd endpoints, "
              "e.g. 'RDMA://10.1.1.1:1234,RDMA://10.1.1.2:1234'");
DEFINE_string(storageEndpoints,
              "",
              "Comma or space separated list of storage ids and endpoints, "
              "e.g. '1@RDMA://10.1.1.1:1234,2@RDMA://10.1.1.2:1234'");
DEFINE_string(monitorEndpoint, "", "Monitor endpoint");
DEFINE_uint32(defaultPKeyIndex, 1, "IB default pkey index");

namespace hf3fs::storage::benchmark {

using namespace hf3fs::storage::client;

std::vector<uint32_t> stringToIntVec(const std::string &str) {
  std::vector<uint32_t> vec;
  std::vector<std::string> substrs;
  boost::split(substrs, str, boost::is_any_of(", "));

  for (auto s : substrs) {
    boost::trim(s);
    if (s.empty()) continue;

    uint32_t n = std::stoul(s);
    vec.push_back(n);
  }

  return vec;
}

bool runBenchmarks() {
  std::vector<std::string> dataPathStrs;
  boost::split(dataPathStrs, FLAGS_dataPaths, boost::is_any_of(", "));

  std::vector<hf3fs::Path> dataPaths;
  dataPaths.reserve(dataPathStrs.size());

  for (auto str : dataPathStrs) {
    boost::trim(str);
    if (str.empty()) continue;

    dataPaths.emplace_back(str);
  }

  std::vector<std::string> endpointRawStrs;
  boost::split(endpointRawStrs, FLAGS_storageEndpoints, boost::is_any_of(", "));

  std::map<NodeId, net::Address> storageEndpoints;

  for (auto str : endpointRawStrs) {
    boost::trim(str);
    if (str.empty()) continue;

    std::vector<std::string> nodeEndpointStrs;
    boost::split(nodeEndpointStrs, str, boost::is_any_of("@"));

    if (nodeEndpointStrs.size() != 2) {
      XLOGF(ERR, "Wrong node endpoint string: {}", str);
      return false;
    }

    auto nodeIdStr = nodeEndpointStrs[0];
    auto endpointStr = nodeEndpointStrs[1];

    auto nodeId = (NodeId)std::stoul(nodeIdStr);
    auto endpoint = net::Address::fromString(endpointStr);
    storageEndpoints[nodeId] = endpoint;
    XLOGF(WARN, "Add storage endpoint: {} @ {}", nodeId, endpoint);
  }

  if (FLAGS_clientMode && storageEndpoints.empty()) {
    XLOGF(ERR, "No storage endpoint specified for client mode");
    return false;
  }

  if (FLAGS_readSize > FLAGS_chunkSizeKB * 1024) {
    XLOGF(ERR, "Read size {} is greater than chunk size {}", FLAGS_readSize, FLAGS_chunkSizeKB * 1024);
    return false;
  }

  auto metaStoreType = static_cast<kv::KVStore::Type>(FLAGS_metaStoreType);

  test::SystemSetupConfig setupConfig = {
      FLAGS_chunkSizeKB * 1024 /*chunkSize*/,
      FLAGS_numChains /*numChains*/,
      FLAGS_numReplicas /*numReplicas*/,
      FLAGS_numStorageNodes /*numStorageNodes*/,
      dataPaths /*dataPaths*/,
      FLAGS_clientConfig,
      FLAGS_serverConfig,
      storageEndpoints,
      FLAGS_serviceLevel,
      FLAGS_listenPort,
      StorageClient::ImplementationType::RPC /*clientImplType*/,
      metaStoreType,
      true /*useFakeMgmtdClient*/,
      !FLAGS_clientMode /*startStorageServer*/,
      false,
  };

  std::vector<std::string> ibvDevices;
  boost::split(ibvDevices, FLAGS_ibvDevices, boost::is_any_of(", "));

  std::vector<std::string> ibnetZones;
  boost::split(ibnetZones, FLAGS_ibnetZones, boost::is_any_of(", "));

  endpointRawStrs.clear();
  boost::split(endpointRawStrs, FLAGS_mgmtdEndpoints, boost::is_any_of(", "));

  std::vector<net::NamedAddress> mgmtdEndpoints;

  for (auto str : endpointRawStrs) {
    boost::trim(str);
    if (str.empty()) continue;

    auto endpoint = net::NamedAddress::from(str).value();
    mgmtdEndpoints.push_back(endpoint);
    XLOGF(WARN, "Add mgmtd endpoint: {}", endpoint);
  }

  StorageBench::Options benchOptions{FLAGS_numChunks,
                                     FLAGS_readSize,
                                     FLAGS_writeSize,
                                     FLAGS_batchSize,
                                     FLAGS_numReadSecs,
                                     FLAGS_numWriteSecs,
                                     FLAGS_clientTimeoutMS,
                                     FLAGS_numCoroutines,
                                     FLAGS_numTestThreads,
                                     FLAGS_randSeed,
                                     (uint16_t)FLAGS_chunkIdPrefix,
                                     FLAGS_benchmarkNetwork,
                                     FLAGS_benchmarkStorage,
                                     FLAGS_ignoreIOError,
                                     FLAGS_injectRandomServerError,
                                     FLAGS_injectRandomClientError,
                                     FLAGS_retryPermanentError,
                                     FLAGS_verifyReadData,
                                     FLAGS_verifyReadChecksum,
                                     FLAGS_verifyWriteChecksum,
                                     FLAGS_randomShuffleChunkIds,
                                     FLAGS_generateTestData,
                                     FLAGS_sparseChunkIds,
                                     FLAGS_statsFilePath,
                                     ibvDevices,
                                     ibnetZones,
                                     mgmtdEndpoints,
                                     FLAGS_clusterId,
                                     FLAGS_chainTableId,
                                     FLAGS_chainTableVersion,
                                     stringToIntVec(FLAGS_chainIds),
                                     stringToIntVec(FLAGS_storageNodeIds),
                                     FLAGS_memoryAlignment,
                                     FLAGS_readOffAlignment,
                                     FLAGS_defaultPKeyIndex,
                                     FLAGS_readBatchSize,
                                     FLAGS_writeBatchSize,
                                     FLAGS_removeBatchSize};

  StorageBench bench(setupConfig, benchOptions);

  if (FLAGS_clusterMode) {
    if (!bench.connect()) {
      XLOGF(WARN, "Failed to connect to cluster");
      return false;
    }
  } else {
    if (!bench.setup()) {
      XLOGF(WARN, "Failed to set up benchmark");
      return false;
    }
  }

  bench.generateChunkIds();

  if (FLAGS_cleanupChunksBeforeBench) {
    bench.cleanup();
  }

  bool runOK = true;

  if (FLAGS_serverMode) {
    XLOGF(WARN, "Waiting...");
    while (true) {
      ::sleep(1);
    }
  } else {
    runOK = bench.run();
  }

  if (FLAGS_truncateChunks) {
    bench.truncate();
  }

  if (FLAGS_cleanupChunks) {
    bench.cleanup();
  }

  bench.teardown();

  return runOK;
}

}  // namespace hf3fs::storage::benchmark

int main(int argc, char **argv) {
  folly::init(&argc, &argv, true);
  hf3fs::monitor::Monitor::Config monitorConfig;

  if (FLAGS_printMetrics || FLAGS_reportMetrics) {
    if (FLAGS_printMetrics) {
      monitorConfig.reporters(0).set_type("log");
    } else if (FLAGS_reportMetrics) {
      monitorConfig.reporters(0).set_type("monitor_collector");
      monitorConfig.reporters(0).monitor_collector().set_remote_ip(FLAGS_monitorEndpoint);
      monitorConfig.set_reporters_length(1);
    }

    auto monitorResult = hf3fs::monitor::Monitor::start(monitorConfig);
    XLOGF_IF(FATAL, !monitorResult, "Failed to start monitor: {}", monitorResult.error());
  }

  bool ok = hf3fs::storage::benchmark::runBenchmarks();

  hf3fs::monitor::Monitor::stop();
  hf3fs::memory::shutdown();

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
