#include "DumpChunkMeta.h"

#include <folly/logging/xlog.h>
#include <fstream>
#include <vector>

#include "AdminEnv.h"
#include "analytics/SerdeObjectReader.h"
#include "analytics/SerdeObjectWriter.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/IEnv.h"
#include "client/cli/common/Utils.h"
#include "client/storage/StorageClient.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("dump-chunkmeta");
  parser.add_argument("-c", "--chain-ids").nargs(argparse::nargs_pattern::any).scan<'u', uint32_t>();
  parser.add_argument("-m", "--chunkmeta-dir").default_value(std::string{"chunkmeta"});
  parser.add_argument("-q", "--parquet-format").default_value(false).implicit_value(true);
  parser.add_argument("-h", "--only-head").default_value(false).implicit_value(true);
  parser.add_argument("-p", "--parallel").default_value(uint32_t{32}).scan<'u', uint32_t>();
  return parser;
}

CoTryTask<Dispatcher::OutputTable> dumpChunkMeta(IEnv &ienv,
                                                 const argparse::ArgumentParser &parser,
                                                 const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  const auto &chainIds = parser.get<std::vector<uint32_t>>("chain-ids");
  const auto &chunkmetaDir = parser.get<std::string>("chunkmeta-dir");
  const auto parallel = std::min(parser.get<uint32_t>("parallel"), std::thread::hardware_concurrency() / 2);
  const auto &parquetFormat = parser.get<bool>("parquet-format");
  const auto &onlyHead = parser.get<bool>("only-head");

  if (boost::filesystem::exists(chunkmetaDir)) {
    XLOGF(CRITICAL, "Output directory for chunk metadata already exists: {}", chunkmetaDir);
    co_return makeError(StatusCode::kInvalidArg);
  }

  boost::system::error_code err{};
  boost::filesystem::create_directories(chunkmetaDir, err);

  if (UNLIKELY(err.failed())) {
    XLOGF(CRITICAL, "Failed to create directory {}, error: {}", chunkmetaDir, err.message());
    co_return makeError(StatusCode::kIOError);
  }

  XLOGF(CRITICAL, "Saving chunk metadata to directory: {}", chunkmetaDir);

  CO_RETURN_AND_LOG_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(/*force=*/true));
  auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
  auto chainInfos = routingInfo->raw()->chains;

  XLOGF(CRITICAL, "Found {} replication chains in {}", chainInfos.size(), routingInfo->raw()->routingInfoVersion);

  std::map<flat::ChainId, flat::ChainInfo> sortedChainInfos;
  for (const auto &[chainId, chainInfo] : chainInfos) sortedChainInfos.emplace(chainId, chainInfo);
  std::set<uint32_t> selectedChainIds(chainIds.begin(), chainIds.end());

  size_t numChainsToSave = selectedChainIds.empty() ? sortedChainInfos.size() : selectedChainIds.size();
  std::atomic_size_t numTargetsSaved = 0;

  auto dumpChunkMeta = [env, chunkmetaDir, parquetFormat, numChainsToSave, &numTargetsSaved](
                           const size_t chainIndex,
                           const flat::ChainId chainId,
                           const flat::TargetId targetId) -> CoTask<bool> {
    XLOGF(INFO, "#{}/{} Getting chunk metadata from target: {}@{}", chainIndex, numChainsToSave, targetId, chainId);

    auto chunkMetadata = co_await env.storageClientGetter()->getAllChunkMetadata(chainId, targetId);

    if (!chunkMetadata) {
      XLOGF(ERR,
            "Failed to get chunk metadata from target: {}@{}, error: {}",
            targetId,
            chainId,
            chunkMetadata.error());
      co_return false;
    }

    time_t timestamp = UtcClock::secondsSinceEpoch();
    ChunkMetaTable metadata{chainId, targetId, timestamp};

    for (const auto &chunkmeta : *chunkMetadata) {
      auto metaChunkId = meta::ChunkId::unpack(chunkmeta.chunkId.data());
      metadata.chunks.push_back({timestamp, chainId, targetId, metaChunkId.inode(), chunkmeta});
    }

    XLOGF(INFO,
          "#{}/{} Found {} chunks on target: {}@{}",
          chainIndex,
          numChainsToSave,
          metadata.chunks.size(),
          targetId,
          chainId);

    auto filePath =
        Path(chunkmetaDir) / fmt::format("{}-{}.meta", uint32_t(metadata.chainId), uint64_t(metadata.targetId));
    bool writeOk = parquetFormat ? metadata.dumpToParquetFile(filePath.replace_extension(".parquet"))
                                 : metadata.dumpToFile(filePath);
    numTargetsSaved += writeOk;

    XLOGF_IF(WARN,
             writeOk,
             "#{}/{} Metadata of {} chunks on {}@{} saved to file: {}",
             chainIndex,
             numChainsToSave,
             metadata.chunks.size(),
             targetId,
             chainId,
             filePath);
    co_return writeOk;
  };

  auto executor = std::make_unique<folly::CPUThreadPoolExecutor>(parallel);
  std::vector<folly::coro::TaskWithExecutor<bool>> tasks;
  size_t chainIndex = 0;

  for (const auto &[chainId, chainInfo] : sortedChainInfos) {
    if (!selectedChainIds.empty() && !selectedChainIds.count(uint32_t(chainId))) continue;

    chainIndex++;

    if (chainInfo.targets.empty()) {
      XLOGF(CRITICAL, "Empty list of targets on {}: {}", chainId, chainInfo);
      co_return makeError(StorageClientCode::kRoutingError);
    }

    for (const auto &target : chainInfo.targets) {
      if (target.publicState == flat::PublicTargetState::SERVING) {
        if (parallel > 0) {
          tasks.push_back(dumpChunkMeta(chainIndex, chainId, target.targetId)
                              .scheduleOn(folly::Executor::getKeepAliveToken(*executor)));
        } else {
          bool ok = co_await dumpChunkMeta(chainIndex, chainId, target.targetId);
          if (!ok) co_return makeError(StatusCode::kIOError);
        }
      } else {
        XLOGF(WARN, "Skip target {}@{} not in serving state: {}", target.targetId, chainId, target);
      }

      if (onlyHead) break;
    }
  }

  if (parallel > 0 && !tasks.empty()) {
    auto results = co_await folly::coro::collectAllWindowed(std::move(tasks), parallel);
    if (!std::all_of(results.begin(), results.end(), [](bool ok) { return ok; })) {
      XLOGF(CRITICAL, "Some of the chunkmeta dump tasks failed");
      co_return makeError(StatusCode::kIOError);
    }
  }

  XLOGF(CRITICAL,
        "Chunk metadata on {} targets from {} chains saved to directory: {}",
        numTargetsSaved.load(),
        numChainsToSave,
        chunkmetaDir);

  co_return table;
}

}  // namespace

bool ChunkMetaTable::dumpToFile(const Path &filePath, bool jsonFormat) const {
  std::ofstream dumpFile{filePath};
  if (UNLIKELY(!dumpFile)) {
    XLOGF(ERR, "Failed to create file: {}", filePath);
    return false;
  }

  std::string buffer;

  if (jsonFormat) {
    buffer = serde::toJsonString(*this, false /*sortKeys*/, true /*prettyFormatting*/);
  } else {
    auto bytes = serde::serializeBytes(*this);
    buffer = std::string(bytes);
  }

  dumpFile.write(buffer.data(), buffer.size());

  if (UNLIKELY(!dumpFile)) {
    XLOGF(ERR, "Failed to write to file: {}", filePath);
    return false;
  }

  return true;
}

bool ChunkMetaTable::loadFromFile(const Path &filePath, bool jsonFormat) {
  std::ifstream inputFile{filePath, std::ios_base::binary};
  if (UNLIKELY(!inputFile)) {
    XLOGF(ERR, "Failed to open file: {}", filePath);
    return false;
  }

  auto fileSize = static_cast<size_t>(boost::filesystem::file_size(filePath));
  std::string buffer;
  buffer.resize(fileSize, '\0');

  inputFile.read(&buffer[0], fileSize);

  if (buffer.size() != fileSize) {
    XLOGF(ERR, "Read size {} not equal to file size {}, file: {}", buffer.size(), fileSize, filePath);
    return false;
  }

  if (jsonFormat) {
    auto result = serde::fromJsonString(*this, buffer);
    return bool(result);
  } else {
    auto result = serde::deserialize(*this, buffer);
    return bool(result);
  }
}

bool ChunkMetaTable::dumpToParquetFile(const Path &filePath) const {
  auto openRes = analytics::SerdeObjectWriter<ChunkMetaRow>::open(filePath);
  if (!openRes) return false;

  auto serdeWriter = std::move(*openRes);

  for (const auto &chunkmeta : chunks) {
    serdeWriter << chunkmeta;
    if (!serdeWriter) break;
  }

  auto timestamp = UtcClock::secondsSinceEpoch();
  serdeWriter << ChunkMetaRow{timestamp, chainId, targetId, meta::InodeId{}, storage::ChunkMeta{}};

  return serdeWriter.ok();
}

bool ChunkMetaTable::loadFromParquetFile(const Path &filePath) {
  auto openRes = analytics::SerdeObjectReader<ChunkMetaRow>::open(filePath);
  if (!openRes) return false;

  auto serdeReader = std::move(*openRes);
  chunks.reserve(serdeReader.numRows());
  timestamp = 0;

  ChunkMetaRow chunkRow;
  while (serdeReader >> chunkRow) {
    chunks.push_back(chunkRow);
    chainId = chunkRow.chainId;
    targetId = chunkRow.targetId;
    timestamp = std::max(timestamp, chunkRow.timestamp);
  }

  return serdeReader.ok();
}

CoTryTask<void> registerDumpChunkMetaHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, dumpChunkMeta);
}

}  // namespace hf3fs::client::cli