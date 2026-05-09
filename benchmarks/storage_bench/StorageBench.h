#pragma once

#include <boost/algorithm/string.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/filesystem/string_file.hpp>
#include <common/utils/UtcTime.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/futures/Barrier.h>
#include <folly/stats/TDigest.h>
#include <numeric>
#include <optional>
#include <random>
#include <vector>
#include <fstream>

#include "common/logging/LogInit.h"
#include "common/net/ib/IBDevice.h"
#include "common/utils/Duration.h"
#include "common/utils/SysResource.h"
#include "tests/lib/UnitTestFabric.h"

namespace hf3fs::storage::benchmark {

using namespace hf3fs::storage::client;

class StorageBench : public test::UnitTestFabric {
 public:
  struct Options {
    const size_t numChunks;
    const size_t readSize;
    const size_t writeSize;
    const size_t batchSize;
    const uint64_t numReadSecs;
    const uint64_t numWriteSecs;
    const uint64_t clientTimeoutMS;
    const size_t numCoroutines;
    const size_t numTestThreads;
    const uint32_t randSeed = 0;
    const uint16_t chunkIdPrefix = 0xFFFF;
    const bool benchmarkNetwork = false;
    const bool benchmarkStorage = false;
    const bool ignoreIOError = false;
    const bool injectRandomServerError = false;
    const bool injectRandomClientError = false;
    const bool retryPermanentError = false;
    const bool verifyReadData = false;
    const bool verifyReadChecksum = false;
    const bool verifyWriteChecksum = true;
    const bool randomShuffleChunkIds = true;
    const bool generateTestData = true;
    const bool sparseChunkIds = true;
    const std::string statsFilePath = "./perfstats.csv";
    const std::vector<std::string> ibvDevices = {};
    const std::vector<std::string> ibnetZones = {};
    const std::vector<net::NamedAddress> mgmtdEndpoints = {};
    const std::string clusterId = kClusterId;
    const uint32_t chainTableId = 0;
    const uint32_t chainTableVersion = 0;
    const std::vector<uint32_t> chainIds = {};
    const std::vector<uint32_t> storageNodeIds = {};
    const size_t memoryAlignment = 1;
    const size_t readOffAlignment = 0;
    const size_t defaultPKeyIndex = 1;
    size_t readBatchSize = 0;
    size_t writeBatchSize = 0;
    size_t removeBatchSize = 0;
  };

 private:
  static constexpr uint32_t kTDigestMaxSize = 1000;

  struct ChunkInfo {
    ChainId chainId;
    ChunkId chunkId;
    size_t size;
  };

  Options benchOptions_;
  std::vector<folly::TDigest> writeLatencyDigests_;
  std::vector<folly::TDigest> readLatencyDigests_;
  folly::CPUThreadPoolExecutor testExecutor_;
  std::atomic_uint64_t numWriteBytes_;
  std::atomic_uint64_t numReadBytes_;
  folly::Random::DefaultGenerator randGen_;
  std::vector<std::vector<ChunkInfo>> chunkInfos_;
  std::vector<size_t> numCreatedChunks_;
  size_t totalNumChunks_;
  double totalChunkGiB_;

 public:
  StorageBench(const test::SystemSetupConfig &setupConfig, const Options &options)
      : UnitTestFabric(setupConfig),
        benchOptions_(options),
        writeLatencyDigests_(benchOptions_.numCoroutines, folly::TDigest(kTDigestMaxSize)),
        readLatencyDigests_(benchOptions_.numCoroutines, folly::TDigest(kTDigestMaxSize)),
        testExecutor_(benchOptions_.numTestThreads),
        numWriteBytes_(0),
        numReadBytes_(0),
        randGen_(folly::Random::create()),
        chunkInfos_(benchOptions_.numCoroutines),
        numCreatedChunks_(benchOptions_.numCoroutines) {
    if (benchOptions_.readBatchSize == 0) benchOptions_.readBatchSize = benchOptions_.batchSize;
    if (benchOptions_.writeBatchSize == 0) benchOptions_.writeBatchSize = benchOptions_.batchSize;
    if (benchOptions_.removeBatchSize == 0) benchOptions_.removeBatchSize = benchOptions_.batchSize;
  }

  void generateChunkIds() {
    static_assert(sizeof(benchOptions_.chunkIdPrefix) == 2);
    uint64_t chunkIdPrefix64 = ((uint64_t)benchOptions_.chunkIdPrefix) << (UINT64_WIDTH - UINT16_WIDTH);
    std::sort(chainIds_.begin(), chainIds_.end());
    static thread_local std::mt19937 generator;
    randGen_.seed(benchOptions_.randSeed);

    XLOGF(WARN,
          "Generating {} chunk ids with prefix {:08X} and random seed {}...",
          totalNumChunks_,
          chunkIdPrefix64,
          benchOptions_.randSeed);

    for (auto &chunkInfos : chunkInfos_) {
      uint64_t instancePrefix = chunkIdPrefix64 | folly::Random::rand64(randGen_);
      XLOGF(DBG3, "Random chunk id prefix {:08X}", instancePrefix);

      chunkInfos.reserve(chainIds_.size() * benchOptions_.numChunks);

      for (auto chainId : chainIds_) {
        for (size_t chunkIndex = 0; chunkIndex < benchOptions_.numChunks; chunkIndex++) {
          if (benchOptions_.sparseChunkIds) {
            uint64_t chunkIdHigh = chunkIdPrefix64 | (folly::Random::rand64(randGen_) & 0x000000FFFFFFFFFF);
            uint64_t chunkIdLow = (folly::Random::rand64(randGen_) << UINT32_WIDTH) + chunkIndex;
            chunkInfos.push_back({chainId, ChunkId(chunkIdHigh, chunkIdLow), 0});
          } else {
            chunkInfos.push_back({chainId, ChunkId(instancePrefix, chunkIndex), 0});
          }
        }
      }

      if (benchOptions_.randomShuffleChunkIds) std::shuffle(chunkInfos.begin(), chunkInfos.end(), generator);
    }
  }

  bool connect() {
    XLOGF(INFO, "Start to connect...");

    if (!setupIBSock()) {
      return false;
    }

    mgmtdClientConfig_.set_mgmtd_server_addresses(benchOptions_.mgmtdEndpoints);
    mgmtdClientConfig_.set_enable_auto_refresh(true);
    mgmtdClientConfig_.set_enable_auto_heartbeat(false);
    mgmtdClientConfig_.set_enable_auto_extend_client_session(true);
    mgmtdClientConfig_.set_auto_refresh_interval(3_s);
    mgmtdClientConfig_.set_auto_heartbeat_interval(3_s);
    mgmtdClientConfig_.set_auto_extend_client_session_interval(3_s);
    mgmtdClientConfig_.set_accept_incomplete_routing_info_during_mgmtd_bootstrapping(false);

    if (!client_.start()) {
      XLOGF(ERR, "Failed to start net client for mgmtd client");
      return false;
    }

    XLOGF(INFO, "Creating mgmtd client...");

    auto stubFactory = std::make_unique<hf3fs::stubs::RealStubFactory<hf3fs::mgmtd::MgmtdServiceStub>>(
        stubs::ClientContextCreator{[this](net::Address addr) { return client_.serdeCtx(addr); }});
    auto mgmtdClient = std::make_unique<hf3fs::client::MgmtdClientForClient>(benchOptions_.clusterId,
                                                                             std::move(stubFactory),
                                                                             mgmtdClientConfig_);

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
                                              /*description=*/fmt::format("StorageBench: {}", *containerHostnameRes),
                                              /*serviceGroups=*/std::vector<flat::ServiceGroupInfo>{},
                                              flat::ReleaseVersion::fromVersionInfo()),
                                          flat::UserInfo{}});
    folly::coro::blockingWait(mgmtdClient->start(&client_.tpg().bgThreadPool().randomPick()));
    mgmtdForClient_ = std::move(mgmtdClient);

    // get routing info

    for (size_t retry = 0; retry < 15; retry++) {
      auto routingInfo = mgmtdForClient_->getRoutingInfo();

      if (routingInfo == nullptr || routingInfo->raw()->chains.empty()) {
        XLOGF(WARN, "Empty routing info, #{} retry...", retry + 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      } else {
        for (const auto &[tableId, tableVersions] : routingInfo->raw()->chainTables) {
          if (tableId == benchOptions_.chainTableId) {
            if (tableVersions.empty()) {
              XLOGF(WARN, "No version found for chain table with id {}", tableId);
              return false;
            }

            XLOGF(INFO, "Found {} version(s) of chain table {}", tableVersions.size(), benchOptions_.chainTableId);

            flat::ChainTable chainTable;

            if (benchOptions_.chainTableVersion > 0) {
              flat::ChainTableVersion tableVersion(benchOptions_.chainTableVersion);
              auto tableIter = tableVersions.find(tableVersion);

              if (tableIter == tableVersions.end()) {
                XLOGF(WARN, "Version {} not found in chain table with id {}", tableVersion, tableId);
                return false;
              }

              chainTable = tableIter->second;
              XLOGF(INFO,
                    "Found version {} of chain table {}: {}",
                    benchOptions_.chainTableVersion,
                    benchOptions_.chainTableId,
                    chainTable.chainTableVersion);
            } else {
              const auto iter = --tableVersions.cend();
              const auto &latestTable = iter->second;
              chainTable = latestTable;
              XLOGF(INFO,
                    "Found latest version of chain table {}: {}",
                    benchOptions_.chainTableId,
                    chainTable.chainTableVersion);
            }

            XLOGF(WARN,
                  "Selected chain table: {}@{} [{}] {} chains",
                  chainTable.chainTableId,
                  chainTable.chainTableVersion,
                  chainTable.desc,
                  chainTable.chains.size());

            if (!benchOptions_.storageNodeIds.empty()) {
              for (const auto &chainId : chainTable.chains) {
                const auto chainInfo = routingInfo->raw()->getChain(chainId);
                for (const auto &target : chainInfo->targets) {
                  const auto targetInfo = routingInfo->raw()->getTarget(target.targetId);
                  auto nodeIter = std::find(benchOptions_.storageNodeIds.begin(),
                                            benchOptions_.storageNodeIds.end(),
                                            *targetInfo->nodeId);
                  if (nodeIter != benchOptions_.storageNodeIds.end()) {
                    chainIds_.push_back(chainId);
                    break;
                  }
                }
              }
            } else if (!benchOptions_.chainIds.empty()) {
              for (const auto &chainId : chainTable.chains) {
                auto chainIter = std::find(benchOptions_.chainIds.begin(), benchOptions_.chainIds.end(), chainId);
                if (chainIter != benchOptions_.chainIds.end()) {
                  chainIds_.push_back(chainId);
                }
              }
            } else {
              chainIds_ = chainTable.chains;
            }

            break;
          }
        }

        if (!chainIds_.empty()) break;
      }
    }

    if (chainIds_.empty()) {
      XLOGF(ERR, "Failed to get chain table with id {}", benchOptions_.chainTableId);
      return false;
    } else {
      XLOGF(WARN, "Selected {} replication chains for benchmark", chainIds_.size());
    }

    // create storage client

    if (setupConfig_.client_config().empty()) {
      XLOGF(ERR, "Storage client config not specified");
      return false;
    }

    auto configRes = clientConfig_.atomicallyUpdate(setupConfig_.client_config(), false /*isHotUpdate*/);
    if (!configRes) {
      XLOGF(ERR, "Cannot load client config from {}, error: {}", setupConfig_.client_config(), configRes.error());
      return false;
    }

    totalNumChunks_ = chainIds_.size() * benchOptions_.numCoroutines * benchOptions_.numChunks;
    totalChunkGiB_ = (double)totalNumChunks_ * setupConfig_.chunk_size() / 1_GB;
    clientConfig_.retry().set_max_retry_time(Duration(std::chrono::milliseconds(benchOptions_.clientTimeoutMS)));
    clientConfig_.net_client().io_worker().ibsocket().set_sl(setupConfig_.service_level());

    XLOGF(INFO, "Creating storage client...");
    storageClient_ = client::StorageClient::create(clientId_, clientConfig_, *mgmtdForClient_);

    return true;
  }

  bool setupIBSock() {
    XLOGF(WARN, "Setting up IB socket...");

    std::vector<net::IBConfig::Subnet> subnets;

    for (const auto &ibnetZoneStr : benchOptions_.ibnetZones) {
      std::vector<std::string> ibnetZoneSubnet;
      boost::split(ibnetZoneSubnet, ibnetZoneStr, boost::is_any_of(":"));

      if (ibnetZoneSubnet.size() != 2) {
        XLOGF(CRITICAL, "Invalid IB zone subnet: {}", ibnetZoneStr);
        return false;
      }

      auto zone = boost::trim_copy(ibnetZoneSubnet[0]);
      auto subnet = boost::trim_copy(ibnetZoneSubnet[1]);

      if (zone.empty() || subnet.empty()) {
        XLOGF(CRITICAL, "Invalid IB zone subnet: {}", ibnetZoneStr);
        return false;
      }

      subnets.emplace_back();
      subnets.back().set_network_zones({zone});
      subnets.back().set_subnet(*net::IBConfig::Network::from(subnet));
      XLOGF(WARN, "Add IB network zone: {} -- {}", zone, subnet);
    }

    net::IBConfig ibConfig;
    ibConfig.set_subnets(subnets);
    ibConfig.set_allow_unknown_zone(false);
    ibConfig.set_default_network_zone("$HF3FS_NETWORK_ZONE");
    ibConfig.set_device_filter(benchOptions_.ibvDevices);
    ibConfig.set_default_pkey_index(benchOptions_.defaultPKeyIndex);

    auto ibResult = net::IBManager::start(ibConfig);
    if (ibResult.hasError()) {
      XLOGF(CRITICAL, "Cannot initialize IB device: {}", ibResult.error());
      return false;
    }

    return true;
  }

  bool setup() {
    XLOGF(WARN, "Setting up benchmark...");

    if (!setupIBSock()) {
      return false;
    }

    bool ok = setUpStorageSystem();

    totalNumChunks_ = chainIds_.size() * benchOptions_.numCoroutines * benchOptions_.numChunks;
    totalChunkGiB_ = (double)totalNumChunks_ * setupConfig_.chunk_size() / 1_GB;
    clientConfig_.retry().set_max_retry_time(Duration(std::chrono::milliseconds(benchOptions_.clientTimeoutMS)));

    return ok;
  }

  void teardown() {
    tearDownStorageSystem();
    net::IBManager::stop();
  }

  void printThroughput(hf3fs::SteadyClock::duration elapsedMicro, double totalGiB) {
    auto elapsedMilli = std::chrono::duration_cast<std::chrono::milliseconds>(elapsedMicro);
    double throughput = totalGiB / (elapsedMilli.count() / 1000.0);
    XLOGF(WARN, "Average throughput: {:.3f}GiB/s, total {:.3f} GiB", throughput, totalGiB);
  }

  void printLatencyDigest(const folly::TDigest &digest) {
    XLOGF(WARN, "latency summary ({} samples)", digest.count());
    XLOGF(WARN, "min: {:10.1f}us", digest.min());
    XLOGF(WARN, "max: {:10.1f}us", digest.max());
    XLOGF(WARN, "avg: {:10.1f}us", digest.mean());
    for (double p : {0.1, 0.2, 0.5, 0.9, 0.95, 0.99}) {
      XLOGF(WARN, "{}%: {:10.1f}us", p * 100.0, digest.estimateQuantile(p));
    }
  }

  void dumpPerfStats(const std::string &testName,
                     const folly::TDigest &digest,
                     hf3fs::SteadyClock::duration elapsedTime,
                     double totalGiB,
                     bool readIO) {
    if (benchOptions_.statsFilePath.empty()) return;

    boost::filesystem::path outFilePath(benchOptions_.statsFilePath);

    if (!boost::filesystem::exists(outFilePath) || boost::filesystem::is_empty(outFilePath)) {
      XLOGF(INFO, "Create a file for perfermance stats at {}", outFilePath);
      std::ofstream outFile(outFilePath);
      if (!outFile) {
          throw std::runtime_error("Failed to open file: " + outFilePath.string());
      }

      outFile << "test name,#storages,#chains,#replicas,concurrency,batch size,"
                 "io size (bytes),effective batch size (batch size / #replicas),elapsed time (us),"
                 "QPS,IOPS,bandwidth (MB/s),latency samples,min latency (us),max latency (us),avg latency (us),"
                 "latency P50 (us),latency P75 (us),latency P90 (us),latency P95 (us),latency P99 (us)\n";

      if (!outFile) {
          throw std::runtime_error("Failed to write to file: " + outFilePath.string());
      }

      outFile.close();
    }

    auto elapsedMicro = std::chrono::duration_cast<std::chrono::microseconds>(elapsedTime);
    double bandwidthMBps = totalGiB * 1024.0 / (elapsedMicro.count() / 1'000'000.0);
    size_t ioSize = readIO ? benchOptions_.readSize : benchOptions_.writeSize;
    size_t batchSize = readIO ? benchOptions_.readBatchSize : benchOptions_.writeBatchSize;
    double iops = bandwidthMBps * 1024.0 * 1024.0 / ioSize;
    double qps = bandwidthMBps * 1024.0 * 1024.0 / (batchSize * ioSize);

    boost::filesystem::ofstream fout(outFilePath, std::ios_base::app);

    fout << fmt::format("{},{},{},{},{},{},{},{:.1f},{},{:.1f},{:.1f},{:.3f},{},{:.1f},{:.1f},{:.1f}",
                        testName,
                        setupConfig_.num_storage_nodes(),
                        setupConfig_.num_chains(),
                        setupConfig_.num_replicas(),
                        benchOptions_.numCoroutines,
                        batchSize,
                        ioSize,
                        double(batchSize) / setupConfig_.num_storage_nodes(),
                        elapsedMicro.count(),
                        qps,
                        iops,
                        bandwidthMBps,
                        digest.count(),
                        digest.min(),
                        digest.max(),
                        digest.mean());

    for (double p : {0.5, 0.75, 0.9, 0.95, 0.99}) {
      fout << fmt::format(",{:.1f}", digest.estimateQuantile(p));
    }

    fout << "\n";
    fout.close();
  }

  CoTask<uint32_t> batchWrite(uint32_t instanceId, size_t writeBatchSize, size_t writeSize, uint32_t numWriteSecs) {
    // create an aligned memory block
    size_t memoryBlockSize = ALIGN_UPPER(setupConfig_.chunk_size(), benchOptions_.memoryAlignment);
    auto memoryBlock = (uint8_t *)folly::aligned_malloc(memoryBlockSize, sysconf(_SC_PAGESIZE));
    auto deleter = [](uint8_t *ptr) { folly::aligned_free(ptr); };
    std::unique_ptr<uint8_t, decltype(deleter)> memoryBlockPtr(memoryBlock, deleter);
    std::memset(memoryBlock, 0xFF, memoryBlockSize);

    if (benchOptions_.verifyReadData) {
      for (size_t byteIndex = 0; byteIndex < memoryBlockSize; byteIndex++) {
        memoryBlock[byteIndex] = byteIndex;
      }
    }

    // register a block of memory
    auto regRes = storageClient_->registerIOBuffer(memoryBlock, memoryBlockSize);

    if (regRes.hasError()) {
      co_return regRes.error().code();
    }

    // create write IOs

    auto ioBuffer = std::move(*regRes);

    WriteOptions options;
    options.set_enableChecksum(benchOptions_.verifyWriteChecksum);
    options.debug().set_bypass_disk_io(benchOptions_.benchmarkNetwork);
    options.debug().set_bypass_rdma_xmit(benchOptions_.benchmarkStorage);
    options.debug().set_inject_random_server_error(benchOptions_.injectRandomServerError);
    options.debug().set_inject_random_client_error(benchOptions_.injectRandomClientError);
    options.retry().set_retry_permanent_error(benchOptions_.retryPermanentError);

    std::vector<double> elapsedMicroSecs;
    uint64_t numWriteBytes = 0;

    std::vector<WriteIO> writeIOs;
    writeIOs.reserve(writeBatchSize);

    auto benchStart = hf3fs::SteadyClock::now();
    std::vector<ChunkInfo> &chunkInfos = chunkInfos_[instanceId];
    size_t &numCreatedChunks = numCreatedChunks_[instanceId];
    size_t seqChunkIndex = 0;

    while (true) {
      if (numWriteSecs) {
        auto accumElapsedSecs =
            std::chrono::duration_cast<std::chrono::seconds>(hf3fs::SteadyClock::now() - benchStart);
        if (accumElapsedSecs >= std::chrono::seconds(numWriteSecs)) break;
      } else {
        if (numCreatedChunks >= chunkInfos.size()) break;
      }

      writeIOs.clear();

      for (size_t writeIndex = 0; writeIndex < writeBatchSize; writeIndex++) {
        auto &[chainId, chunkId, chunkSize] = chunkInfos[seqChunkIndex++ % chunkInfos.size()];
        size_t writeOffset = 0;
        size_t writeLength = 0;

        if (chunkSize < setupConfig_.chunk_size()) {
          writeOffset = chunkSize;
          writeLength = std::min(writeSize, setupConfig_.chunk_size() - writeOffset);
          chunkSize += writeLength;
          numCreatedChunks += chunkSize == setupConfig_.chunk_size();
        } else {
          writeOffset = folly::Random::rand32(0, setupConfig_.chunk_size() - writeSize);
          writeLength = writeSize;
        }

        auto writeIO = storageClient_->createWriteIO(chainId,
                                                     chunkId,
                                                     writeOffset,
                                                     writeLength,
                                                     setupConfig_.chunk_size(),
                                                     &memoryBlock[writeOffset],
                                                     &ioBuffer);
        writeIOs.push_back(std::move(writeIO));
        numWriteBytes += writeLength;
      }

      auto rpcStart = hf3fs::SteadyClock::now();

      co_await storageClient_->batchWrite(writeIOs, flat::UserInfo(), options);

      auto elapsedMicro = std::chrono::duration_cast<std::chrono::microseconds>(hf3fs::SteadyClock::now() - rpcStart);
      elapsedMicroSecs.push_back(elapsedMicro.count());

      if (!benchOptions_.ignoreIOError) {
        for (const auto &writeIO : writeIOs) {
          if (writeIO.result.lengthInfo.hasError()) {
            XLOGF(ERR, "Error in write result: {}", writeIO.result);
            co_return writeIO.result.lengthInfo.error().code();
          }
          if (writeIO.length != *writeIO.result.lengthInfo) {
            XLOGF(ERR, "Unexpected write length: {} != {}", *writeIO.result.lengthInfo, writeIO.length);
            co_return StorageClientCode::kRemoteIOError;
          }
        }
      }
    }

    folly::TDigest digest;
    writeLatencyDigests_[instanceId] = digest.merge(elapsedMicroSecs);
    numWriteBytes_ += numWriteBytes;

    co_return StatusCode::kOK;
  }

  CoTask<uint32_t> batchRead(uint32_t instanceId) {
    // create an aligned memory block
    size_t alignedBufSize = ALIGN_UPPER(std::max(size_t(1), benchOptions_.readSize), benchOptions_.memoryAlignment);
    size_t memoryBlockSize = alignedBufSize * benchOptions_.readBatchSize;
    auto memoryBlock = (uint8_t *)folly::aligned_malloc(memoryBlockSize, sysconf(_SC_PAGESIZE));
    auto deleter = [](uint8_t *ptr) { folly::aligned_free(ptr); };
    std::unique_ptr<uint8_t, decltype(deleter)> memoryBlockPtr(memoryBlock, deleter);
    std::memset(memoryBlock, 0, memoryBlockSize);

    // register a block of memory
    auto regRes = storageClient_->registerIOBuffer(memoryBlock, memoryBlockSize);

    if (regRes.hasError()) {
      co_return regRes.error().code();
    }

    std::vector<uint8_t> expectedChunkData(setupConfig_.chunk_size());

    if (benchOptions_.verifyReadData) {
      for (size_t byteIndex = 0; byteIndex < expectedChunkData.size(); byteIndex++) {
        expectedChunkData[byteIndex] = byteIndex;
      }
    }

    // create read IOs

    auto ioBuffer = std::move(*regRes);

    ReadOptions options;
    options.set_enableChecksum(benchOptions_.verifyReadChecksum);
    options.debug().set_bypass_disk_io(benchOptions_.benchmarkNetwork);
    options.debug().set_bypass_rdma_xmit(benchOptions_.benchmarkStorage);
    options.debug().set_inject_random_server_error(benchOptions_.injectRandomServerError);
    options.debug().set_inject_random_client_error(benchOptions_.injectRandomClientError);
    options.retry().set_retry_permanent_error(benchOptions_.retryPermanentError);

    std::vector<double> elapsedMicroSecs;
    uint64_t numReadBytes = 0;
    size_t offsetAlignment =
        benchOptions_.readOffAlignment ? benchOptions_.readOffAlignment : std::max(size_t(1), benchOptions_.readSize);

    std::vector<client::ReadIO> readIOs;
    readIOs.reserve(benchOptions_.readBatchSize);

    auto benchStart = hf3fs::SteadyClock::now();
    std::vector<ChunkInfo> &chunkInfos = chunkInfos_[instanceId];

    while (true) {
      auto accumElapsedSecs = std::chrono::duration_cast<std::chrono::seconds>(hf3fs::SteadyClock::now() - benchStart);
      if (accumElapsedSecs >= std::chrono::seconds(benchOptions_.numReadSecs)) break;

      readIOs.clear();

      for (size_t readIndex = 0; readIndex < benchOptions_.readBatchSize; readIndex++) {
        uint64_t randChunkIndex = folly::Random::rand64(0, chunkInfos.size());
        const auto &[chainId, chunkId, chunkSize] = chunkInfos[randChunkIndex];
        uint32_t offset = folly::Random::rand32(0, setupConfig_.chunk_size() - benchOptions_.readSize);
        uint32_t alignedOffset = ALIGN_LOWER(offset, offsetAlignment);
        auto readIO = storageClient_->createReadIO(chainId,
                                                   chunkId,
                                                   alignedOffset /*offset*/,
                                                   benchOptions_.readSize /*length*/,
                                                   &memoryBlock[readIndex * alignedBufSize],
                                                   &ioBuffer);
        readIOs.push_back(std::move(readIO));
        numReadBytes += benchOptions_.readSize;
      }

      auto rpcStart = hf3fs::SteadyClock::now();

      co_await storageClient_->batchRead(readIOs, flat::UserInfo(), options);

      auto elapsedMicro = std::chrono::duration_cast<std::chrono::microseconds>(hf3fs::SteadyClock::now() - rpcStart);
      elapsedMicroSecs.push_back(elapsedMicro.count());

      if (!benchOptions_.ignoreIOError) {
        for (const auto &readIO : readIOs) {
          if (readIO.result.lengthInfo.hasError()) {
            XLOGF(ERR, "Error in read result: {}", readIO.result);
            co_return readIO.result.lengthInfo.error().code();
          }
          if (readIO.length != *readIO.result.lengthInfo) {
            XLOGF(ERR, "Unexpected read length: {} != {}", *readIO.result.lengthInfo, readIO.length);
            co_return StorageClientCode::kRemoteIOError;
          }
        }
      }

      if (benchOptions_.verifyReadData) {
        for (const auto &readIO : readIOs) {
          auto diffPos = std::mismatch(&readIO.data[0], &readIO.data[readIO.length], &expectedChunkData[readIO.offset]);
          uint32_t byteIndex = diffPos.first - &readIO.data[0];
          if (byteIndex < readIO.length) {
            XLOGF(ERR,
                  "Wrong data at bytes index {} and chunk offset {}: data {:#x} != expected {:#x}",
                  byteIndex,
                  readIO.offset + byteIndex,
                  *diffPos.first,
                  *diffPos.second);
            co_return StorageClientCode::kFoundBug;
          }
        }
      }
    }

    folly::TDigest digest;
    readLatencyDigests_[instanceId] = digest.merge(elapsedMicroSecs);
    numReadBytes_ += numReadBytes;

    co_return StatusCode::kOK;
  }

  uint32_t generateChunks() {
    XLOGF(WARN, "Generating {} test chunks ({:.3f} GiB)...", totalNumChunks_, totalChunkGiB_);

    auto testStart = hf3fs::SteadyClock::now();
    std::vector<folly::SemiFuture<uint32_t>> writeTasks;
    numWriteBytes_ = 0;

    size_t writeBatchSize =
        std::max(benchOptions_.writeBatchSize,
                 clientConfig_.traffic_control().write().max_concurrent_requests() / benchOptions_.numCoroutines);

    for (size_t instanceId = 0; instanceId < benchOptions_.numCoroutines; instanceId++) {
      writeTasks.push_back(batchWrite(instanceId, writeBatchSize, setupConfig_.chunk_size(), 0 /*numWriteSecs*/)
                               .scheduleOn(folly::Executor::getKeepAliveToken(testExecutor_))
                               .start());
    }

    auto results = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(writeTasks)));

    for (auto res : results) {
      if (res != StatusCode::kOK) {
        XLOGF(WARN, "Test task failed with status code: {}", res);
        return res;
      }
    }

    auto elapsedTime = hf3fs::SteadyClock::now() - testStart;
    double totalGiB = (double)numWriteBytes_ / 1_GB;
    printThroughput(elapsedTime, totalGiB);

    auto mergedDigest = folly::TDigest::merge(writeLatencyDigests_);
    printLatencyDigest(mergedDigest);

    return StatusCode::kOK;
  }

  uint32_t runWriteBench() {
    XLOGF(WARN,
          "Running write benchmark ({} secs, {} chunks, {:.3f} GiB)...",
          benchOptions_.numWriteSecs,
          totalNumChunks_,
          totalChunkGiB_);

    auto testStart = hf3fs::SteadyClock::now();
    std::vector<folly::SemiFuture<uint32_t>> writeTasks;
    numWriteBytes_ = 0;

    for (size_t instanceId = 0; instanceId < benchOptions_.numCoroutines; instanceId++) {
      writeTasks.push_back(
          batchWrite(instanceId, benchOptions_.writeBatchSize, benchOptions_.writeSize, benchOptions_.numWriteSecs)
              .scheduleOn(folly::Executor::getKeepAliveToken(testExecutor_))
              .start());
    }

    auto results = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(writeTasks)));

    for (auto res : results) {
      if (res != StatusCode::kOK) {
        XLOGF(WARN, "Test task failed with status code: {}", res);
        return res;
      }
    }

    auto elapsedTime = hf3fs::SteadyClock::now() - testStart;
    double totalGiB = (double)numWriteBytes_ / 1_GB;
    printThroughput(elapsedTime, totalGiB);

    auto mergedDigest = folly::TDigest::merge(writeLatencyDigests_);
    printLatencyDigest(mergedDigest);

    dumpPerfStats("batch write", mergedDigest, elapsedTime, totalGiB, false /*readIO*/);

    return StatusCode::kOK;
  }

  uint32_t runReadBench() {
    XLOGF(WARN, "Running read benchmark ({} secs)...", benchOptions_.numReadSecs);

    auto testStart = hf3fs::SteadyClock::now();
    std::vector<folly::SemiFuture<uint32_t>> readTasks;
    numReadBytes_ = 0;

    for (size_t instanceId = 0; instanceId < benchOptions_.numCoroutines; instanceId++) {
      readTasks.push_back(batchRead(instanceId).scheduleOn(folly::Executor::getKeepAliveToken(testExecutor_)).start());
    }

    auto results = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(readTasks)));

    for (auto res : results) {
      if (res != StatusCode::kOK) {
        XLOGF(WARN, "Test task failed with status code: {}", res);
        return res;
      }
    }

    auto elapsedTime = hf3fs::SteadyClock::now() - testStart;
    double totalGiB = (double)numReadBytes_ / 1_GB;
    printThroughput(elapsedTime, totalGiB);

    auto mergedDigest = folly::TDigest::merge(readLatencyDigests_);
    printLatencyDigest(mergedDigest);

    dumpPerfStats("batch read", mergedDigest, elapsedTime, totalGiB, false /*readIO*/);

    return StatusCode::kOK;
  }

  uint32_t cleanup() {
    XLOGF(WARN, "Clean up chunks...");

    std::vector<folly::SemiFuture<uint32_t>> removeTasks;

    for (size_t instanceId = 0; instanceId < benchOptions_.numCoroutines; instanceId++) {
      auto batchRemove = [this](size_t instanceId) -> folly::coro::Task<uint32_t> {
        std::vector<client::RemoveChunksOp> removeOps;
        size_t totalNumChunksRemoved = 0;

        for (const auto &[chainId, chunkId, chunkSize] : chunkInfos_[instanceId]) {
          removeOps.push_back(storageClient_->createRemoveOp(chainId, chunkId, ChunkId(chunkId, 1)));

          if (removeOps.size() >= benchOptions_.removeBatchSize) {
            WriteOptions options;
            options.debug().set_inject_random_server_error(benchOptions_.injectRandomServerError);
            options.debug().set_inject_random_client_error(benchOptions_.injectRandomClientError);
            options.retry().set_retry_permanent_error(benchOptions_.retryPermanentError);

            co_await storageClient_->removeChunks(removeOps, flat::UserInfo(), options);

            for (const auto &removeOp : removeOps) {
              if (removeOp.result.statusCode.hasError()) {
                XLOGF(WARN, "Remove operation failed with error: {}", removeOp.result.statusCode.error());
                co_return removeOp.result.statusCode.error().code();
              }

              XLOGF_IF(DBG5,
                       removeOp.result.numChunksRemoved != 1,
                       "{} chunks removed in range {}",
                       removeOp.result.numChunksRemoved,
                       removeOp.chunkRange());
              totalNumChunksRemoved += removeOp.result.numChunksRemoved;
            }

            removeOps.clear();
          }
        }

        XLOGF(WARN, "{} chunks removed by instance #{}", totalNumChunksRemoved, instanceId);
        co_return StatusCode::kOK;
      };

      removeTasks.push_back(
          batchRemove(instanceId).scheduleOn(folly::Executor::getKeepAliveToken(testExecutor_)).start());
    }

    auto results = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(removeTasks)));

    for (auto res : results) {
      if (res != StatusCode::kOK) {
        XLOGF(WARN, "Test task failed with status code: {}", res);
        return res;
      }
    }

    return StatusCode::kOK;
  };

  uint32_t truncate() {
    XLOGF(WARN, "Truncate chunks...");

    std::vector<folly::SemiFuture<uint32_t>> truncateTasks;

    for (size_t instanceId = 0; instanceId < benchOptions_.numCoroutines; instanceId++) {
      auto batchTruncate = [this](size_t instanceId) -> folly::coro::Task<uint32_t> {
        std::vector<client::TruncateChunkOp> truncateOps;

        for (const auto &[chainId, chunkId, chunkSize] : chunkInfos_[instanceId]) {
          truncateOps.push_back(storageClient_->createTruncateOp(chainId, chunkId, 0, setupConfig_.chunk_size()));

          if (truncateOps.size() >= benchOptions_.writeBatchSize) {
            WriteOptions options;
            options.debug().set_inject_random_server_error(benchOptions_.injectRandomServerError);
            options.debug().set_inject_random_client_error(benchOptions_.injectRandomClientError);
            options.retry().set_retry_permanent_error(benchOptions_.retryPermanentError);

            co_await storageClient_->truncateChunks(truncateOps, flat::UserInfo(), options);

            for (const auto &truncateOp : truncateOps) {
              if (truncateOp.result.lengthInfo.hasError()) {
                XLOGF(WARN, "Truncate operation failed with error: {}", truncateOp.result.lengthInfo.error());
                co_return truncateOp.result.lengthInfo.error().code();
              }
            }

            truncateOps.clear();
          }
        }

        co_return StatusCode::kOK;
      };

      truncateTasks.push_back(
          batchTruncate(instanceId).scheduleOn(folly::Executor::getKeepAliveToken(testExecutor_)).start());
    }

    auto results = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(truncateTasks)));

    for (auto res : results) {
      if (res != StatusCode::kOK) {
        XLOGF(WARN, "Test task failed with status code: {}", res);
        return res;
      }
    }

    return StatusCode::kOK;
  };

  bool run() {
    if (benchOptions_.numWriteSecs > 0)
      if (runWriteBench() != StatusCode::kOK) return false;
    if (benchOptions_.generateTestData)
      if (generateChunks() != StatusCode::kOK) return false;
    if (benchOptions_.numReadSecs > 0)
      if (runReadBench() != StatusCode::kOK) return false;
    return true;
  }

  uint64_t getWriteBytes() { return numWriteBytes_; }

  uint64_t getReadBytes() { return numReadBytes_; }
};

}  // namespace hf3fs::storage::benchmark
