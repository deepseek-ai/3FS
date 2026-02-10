#include <folly/logging/xlog.h>
#include <vector>

#include "AdminEnv.h"
#include "DumpInodes.h"
#include "FindOrphanedChunks.h"
#include "client/cli/admin/DumpChunkMeta.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/IEnv.h"
#include "client/cli/common/Utils.h"
#include "client/storage/StorageClient.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("remove-chunks");
  parser.add_argument("-n", "--num-inodes-perfile").default_value(uint32_t{10'000'000}).scan<'u', uint32_t>();
  parser.add_argument("-i", "--inode-dir").default_value(std::string{"inodes2"});
  parser.add_argument("-o", "--orphaned-path").default_value(std::string{"orphaned"});
  parser.add_argument("-r", "--do-remove").default_value(false).implicit_value(true);
  parser.add_argument("-q", "--parquet-format").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> removeChunks(IEnv &ienv,
                                                const argparse::ArgumentParser &parser,
                                                const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  ENSURE_USAGE(env.mgmtdClientGetter);
  ENSURE_USAGE(env.kvEngineGetter);
  Dispatcher::OutputTable table;

  // dump latest inodes

  const auto &numInodesPerFile = parser.get<uint32_t>("num-inodes-perfile");
  const auto &inodeDir = parser.get<std::string>("inode-dir");
  const auto &parquetFormat = parser.get<bool>("parquet-format");

  if (boost::filesystem::exists(inodeDir)) {
    XLOGF(CRITICAL, "Output directory for inodes already exists: {}", inodeDir);
    co_return makeError(StatusCode::kInvalidArg);
  }

  auto dumpRes = co_await dumpInodesFromFdb(env.kvEngineGetter(), numInodesPerFile, inodeDir, parquetFormat);
  if (!dumpRes) co_return makeError(dumpRes.error());

  // load the inode dump

  std::time_t inodeDumpTime(std::time(nullptr));
  auto uniqInodeIds = co_await loadInodeFromFiles(inodeDir,
                                                  {},
                                                  uint32_t{std::max(1U, std::thread::hardware_concurrency() / 2)},
                                                  parquetFormat,
                                                  &inodeDumpTime);
  if (!uniqInodeIds) co_return makeError(uniqInodeIds.error());

  const auto &doRemove = parser.get<bool>("do-remove");
  const auto &orphanedPath = parser.get<std::string>("orphaned-path");

  std::vector<Path> orphanedChunkPaths = listFilesFromPath(orphanedPath);
  size_t totalOrphanedChunks = 0;
  size_t totalRemainingOrphanedChunks = 0;
  size_t totalRemovedOrphanedChunks = 0;

  XLOGF(CRITICAL, "Removing orphaned chunks from {} files in path: {}", orphanedChunkPaths.size(), orphanedPath);

  for (size_t orphanedChunkPathIndex = 0; orphanedChunkPathIndex < orphanedChunkPaths.size();
       orphanedChunkPathIndex++) {
    auto orphanedChunkPath = orphanedChunkPaths[orphanedChunkPathIndex];
    ChunkMetaTable orphanedChunkmeta;

    // load orphaned chunks
    bool ok = parquetFormat ? orphanedChunkmeta.loadFromParquetFile(orphanedChunkPath)
                            : orphanedChunkmeta.loadFromFile(orphanedChunkPath, true /*jsonFormat*/);
    if (!ok) {
      XLOGF(FATAL, "Failed to load orphaned chunks in file: {}", orphanedChunkPath);
      co_return makeError(StatusCode::kIOError);
    }

    if (orphanedChunkmeta.timestamp >= inodeDumpTime) {
      XLOGF(CRITICAL,
            "Orphaned chunk metadata dump time '{:%c}' >= inode snapshot time '{:%c}', skipping file: {}",
            fmt::localtime(orphanedChunkmeta.timestamp),
            fmt::localtime(inodeDumpTime),
            orphanedChunkPath);
      co_return makeError(StatusCode::kInvalidArg);
    }

    totalOrphanedChunks += orphanedChunkmeta.chunks.size();

    std::vector<uint8_t> readBuffer(orphanedChunkmeta.chunks.size());
    auto ioBuffer = env.storageClientGetter()->registerIOBuffer(&readBuffer[0], readBuffer.size());
    if (!ioBuffer) co_return makeError(ioBuffer.error());

    std::vector<storage::client::ReadIO> readIOs;
    readIOs.reserve(orphanedChunkmeta.chunks.size());

    for (const auto &orphanedChunkRow : orphanedChunkmeta.chunks) {
      auto metaChunkId = meta::ChunkId::unpack(orphanedChunkRow.chunkmeta.chunkId.data());
      auto metaInodeId = metaChunkId.inode();

      // double check the inode of orphaned chunks
      if (uniqInodeIds->count(metaInodeId)) {
        XLOGF(CRITICAL,
              "Stop removing since inode {} of chunk {} still exists, file: {}",
              metaInodeId,
              metaChunkId,
              orphanedChunkPath);
        co_return makeError(StatusCode::kInvalidArg);
      }

      auto readIO = env.storageClientGetter()->createReadIO(orphanedChunkRow.chainId,
                                                            orphanedChunkRow.chunkmeta.chunkId,
                                                            0 /* offset*/,
                                                            1 /* length*/,
                                                            &readBuffer[readIOs.size()],
                                                            &(*ioBuffer));
      readIOs.push_back(std::move(readIO));
    }

    XLOGF_IF(FATAL,
             readIOs.size() != orphanedChunkmeta.chunks.size(),
             "Num of read IOs {} not equal to num of orphaned chunks {}, file: {}",
             readIOs.size(),
             orphanedChunkmeta.chunks.size(),
             orphanedChunkPath);

    // send batch read request to check if the orphaned chunks still there
    co_await env.storageClientGetter()->batchRead(readIOs, flat::UserInfo{});

    std::vector<storage::client::RemoveChunksOp> removeOps;
    removeOps.reserve(orphanedChunkmeta.chunks.size());

    for (const auto &readIO : readIOs) {
      if (readIO.statusCode() == StorageClientCode::kChunkNotFound) {
        XLOGF(DBG, "Orphaned chunk {} on {} does not exist, skipping", readIO.chunkId, readIO.routingTarget.chainId);
      } else {
        auto removeOp = env.storageClientGetter()->createRemoveOp(readIO.routingTarget.chainId,
                                                                  storage::ChunkId(readIO.chunkId),
                                                                  storage::ChunkId(readIO.chunkId, 1),
                                                                  1 /*maxNumChunkIdsToProcess*/);
        removeOps.push_back(std::move(removeOp));

        XLOGF_IF(WARN,
                 readIO.statusCode() != StatusCode::kOK,
                 "Failed to read orphaned chunk {} on {} with error {}, removing it anyway",
                 readIO.chunkId,
                 readIO.routingTarget.chainId,
                 readIO.status());
      }
    }

    totalRemainingOrphanedChunks += removeOps.size();

    XLOGF_IF(WARN,
             (!removeOps.empty() && removeOps.size() < readIOs.size()),
             "Only {} out of {} orphaned chunks on {} still exist, file: {}",
             removeOps.size(),
             readIOs.size(),
             orphanedChunkmeta.chainId,
             orphanedChunkPath);

    if (!doRemove) {
      XLOGF(WARN, "Skip removing {} orphaned chunks in dry run mode, file: {}", removeOps.size(), orphanedChunkPath);
      continue;
    }

    // send batch remove request
    co_await env.storageClientGetter()->removeChunks(removeOps, flat::UserInfo{});

    size_t removedChunks = std::accumulate(removeOps.begin(), removeOps.end(), size_t{0}, [](size_t s, const auto &op) {
      return s + op.numProcessedChunks();
    });

    totalRemovedOrphanedChunks += removedChunks;

    XLOGF(WARN,
          "#{}/{} Removed {}/{} orphaned chunks on {}, file: {}",
          orphanedChunkPathIndex,
          orphanedChunkPaths.size(),
          removedChunks,
          removeOps.size(),
          orphanedChunkmeta.chainId,
          orphanedChunkPath);
  }

  XLOGF(CRITICAL,
        "In total removed {} of {} remaining orphaned chunks among {} chunks found in directory: {}",
        totalRemovedOrphanedChunks,
        totalRemainingOrphanedChunks,
        totalOrphanedChunks,
        orphanedPath);

  co_return table;
}

}  // namespace

CoTryTask<void> registerRemoveChunksHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, removeChunks);
}

}  // namespace hf3fs::client::cli