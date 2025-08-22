#include "FindOrphanedChunks.h"

#include <folly/logging/xlog.h>
#include <vector>

#include "AdminEnv.h"
#include "DumpChunkMeta.h"
#include "DumpInodes.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/IEnv.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {

namespace {

auto getParser() {
  argparse::ArgumentParser parser("find-orphaned-chunks");
  parser.add_argument("-i", "--inode-path").default_value(std::string{"inodes"});
  parser.add_argument("-m", "--chunkmeta-path").default_value(std::string{"chunkmeta"});
  parser.add_argument("-n", "--inode-ids").nargs(argparse::nargs_pattern::any).scan<'x', uint64_t>();
  parser.add_argument("-o", "--orphaned-dir").default_value(std::string{"orphaned"});
  parser.add_argument("-x", "--ignore-chunkid-prefix").default_value(std::string{"F"});
  parser.add_argument("-v", "--only-chunkid-prefix").default_value(std::string{""});
  parser.add_argument("-S", "--skip-safety-check").default_value(false).implicit_value(true);
  parser.add_argument("-q", "--parquet-format").default_value(false).implicit_value(true);
  parser.add_argument("-p", "--parallel").default_value(uint32_t{32}).scan<'u', uint32_t>();
  return parser;
}

CoTryTask<Dispatcher::OutputTable> findOrphanedChunks(IEnv &ienv,
                                                      const argparse::ArgumentParser &parser,
                                                      const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  ENSURE_USAGE(env.mgmtdClientGetter);
  Dispatcher::OutputTable table;

  const auto &inodePath = parser.get<std::string>("inode-path");
  const auto &chunkmetaPath = parser.get<std::string>("chunkmeta-path");
  const auto &inodeInt64Ids = parser.get<std::vector<uint64_t>>("inode-ids");
  const auto &orphanedDir = parser.get<std::string>("orphaned-dir");
  const auto &ignoreChunkIdPrefix = parser.get<std::string>("ignore-chunkid-prefix");
  const auto &onlyChunkIdPrefix = parser.get<std::string>("only-chunkid-prefix");
  const auto &parquetFormat = parser.get<bool>("parquet-format");
  const auto &skipSafetyCheck = parser.get<bool>("skip-safety-check");
  const auto parallel = std::min(parser.get<uint32_t>("parallel"), std::thread::hardware_concurrency() / 2);

  if (boost::filesystem::exists(orphanedDir)) {
    XLOGF(CRITICAL, "Output directory for orphaned chunks already exists: {}", orphanedDir);
    co_return makeError(StatusCode::kInvalidArg);
  }

  boost::system::error_code err{};
  boost::filesystem::create_directories(orphanedDir, err);

  if (UNLIKELY(err.failed())) {
    XLOGF(CRITICAL, "Failed to create directory {}, error: {}", orphanedDir, err.message());
    co_return makeError(StatusCode::kIOError);
  }

  std::time_t inodeDumpTime(std::time(nullptr));
  robin_hood::unordered_set<meta::InodeId> inodeIdsToPeek;
  for (const auto &n : inodeInt64Ids) inodeIdsToPeek.insert(meta::InodeId{n});

  auto uniqInodeIds = co_await loadInodeFromFiles(inodePath, inodeIdsToPeek, parallel, parquetFormat, &inodeDumpTime);
  if (!uniqInodeIds) {
    XLOGF(FATAL, "Failed to load inodes from directory {}, error: {}", inodePath, uniqInodeIds.error());
    co_return makeError(uniqInodeIds.error());
  }

  if (!inodeIdsToPeek.empty()) {
    XLOGF(CRITICAL, "Completed to find {} inodes in path: {}", inodeIdsToPeek.size(), inodePath);
    co_return table;
  }

  std::vector<Path> chunkmetaFilePaths = listFilesFromPath(chunkmetaPath);
  XLOGF(CRITICAL, "Processing {} chunk metadata files in path: {}", chunkmetaFilePaths.size(), chunkmetaPath);

  std::atomic_size_t numOrphanedChunks = 0;
  std::atomic_size_t numIgnoredOrphanedChunks = 0;
  std::atomic_size_t numProcessedChunks = 0;
  std::atomic_size_t sizeOfOrphanedChunks = 0;
  std::atomic_size_t sizeOfIgnoredOrphanedChunks = 0;
  auto executor = std::make_unique<folly::CPUThreadPoolExecutor>(parallel);
  CountDownLatch<folly::fibers::Baton> loadChunkmetaDone(chunkmetaFilePaths.size());

  for (size_t chunkmetaFileIndex = 0; chunkmetaFileIndex < chunkmetaFilePaths.size(); chunkmetaFileIndex++) {
    executor->add([&, chunkmetaFileIndex, chunkmetaFilePath = chunkmetaFilePaths[chunkmetaFileIndex]]() {
      auto guard = folly::makeGuard([&loadChunkmetaDone]() { loadChunkmetaDone.countDown(); });
      ChunkMetaTable metadata;

      bool ok =
          parquetFormat ? metadata.loadFromParquetFile(chunkmetaFilePath) : metadata.loadFromFile(chunkmetaFilePath);
      if (!ok) {
        XLOGF(FATAL, "Failed to load file: {}", chunkmetaFilePath);
        return false;
      }

      if (!skipSafetyCheck && metadata.timestamp >= inodeDumpTime) {
        XLOGF(FATAL,
              "Chunk metadata dump time '{:%c}' >= inode snapshot time '{:%c}', skipping file: {}",
              fmt::localtime(metadata.timestamp),
              fmt::localtime(inodeDumpTime),
              chunkmetaFilePath);
        return false;
      }

      ChunkMetaTable orphanedMetadata{.chainId = metadata.chainId,
                                      .targetId = metadata.targetId,
                                      .timestamp = metadata.timestamp};

      for (const auto &chunk : metadata.chunks) {
        const auto &chunkmeta = chunk.chunkmeta;

        if (chunkmeta.chunkId != storage::ChunkId{} && !uniqInodeIds->count(chunk.inodeId)) {
          if ((!ignoreChunkIdPrefix.empty() && chunkmeta.chunkId.toString().starts_with(ignoreChunkIdPrefix)) ||
              (!onlyChunkIdPrefix.empty() && !chunkmeta.chunkId.toString().starts_with(onlyChunkIdPrefix))) {
            XLOGF(DBG, "Ignore an orphaned chunk on {}@{}: {}", chunkmeta, metadata.targetId, metadata.chainId);
            numIgnoredOrphanedChunks++;
            sizeOfIgnoredOrphanedChunks += chunkmeta.length;
          } else {
            XLOGF(DBG, "Found an orphaned chunk on {}@{}: {}", chunkmeta, metadata.targetId, metadata.chainId);
            numOrphanedChunks++;
            sizeOfOrphanedChunks += chunkmeta.length;
            orphanedMetadata.chunks.push_back(chunk);
          }
        }
      }

      if (!orphanedMetadata.chunks.empty()) {
        auto orphanedChunkFilePath = Path(orphanedDir) / chunkmetaFilePath.filename();

        bool writeOk = parquetFormat
                           ? orphanedMetadata.dumpToParquetFile(orphanedChunkFilePath.replace_extension(".parquet"))
                           : orphanedMetadata.dumpToFile(orphanedChunkFilePath, true /*jsonFormat*/);
        if (writeOk) {
          XLOGF(CRITICAL,
                "{} orphaned chunks on {}@{} saved to file: {}",
                orphanedMetadata.chunks.size(),
                orphanedMetadata.targetId,
                orphanedMetadata.chainId,
                orphanedChunkFilePath);
        }
      }

      XLOGF(WARN,
            "#{}/{} Processed metadata of {} chunks in file: {}",
            chunkmetaFileIndex + 1,
            chunkmetaFilePaths.size(),
            metadata.chunks.size(),
            chunkmetaFilePath);

      numProcessedChunks += metadata.chunks.size();
      return true;
    });
  }

  co_await loadChunkmetaDone.wait();
  executor->join();

  XLOGF(CRITICAL,
        "In total {}/{} orphaned chunks ({:.3f} GB) ignored",
        numIgnoredOrphanedChunks.load(),
        numProcessedChunks.load(),
        double(sizeOfIgnoredOrphanedChunks.load()) / 1_GB);
  XLOGF(CRITICAL,
        "In total {}/{} orphaned chunks ({:.3f} GB) saved to directory: {}",
        numOrphanedChunks.load(),
        numProcessedChunks.load(),
        double(sizeOfOrphanedChunks.load()) / 1_GB,
        orphanedDir);

  co_return table;
}

}  // namespace

CoTryTask<void> registerFindOrphanedChunksHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, findOrphanedChunks);
}

}  // namespace hf3fs::client::cli