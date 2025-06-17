#include "DumpInodes.h"

#include <algorithm>
#include <cstdint>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <fstream>
#include <memory>
#include <utility>
#include <vector>

#include "AdminEnv.h"
#include "analytics/SerdeObjectReader.h"
#include "analytics/SerdeObjectWriter.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/IEnv.h"
#include "client/cli/common/Utils.h"
#include "common/logging/LogHelper.h"
#include "common/utils/Result.h"
#include "common/utils/Utf8.h"
#include "meta/event/Scan.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("dump-inodes");
  parser.add_argument("-n", "--num-inodes-perfile").default_value(uint32_t{10'000'000}).scan<'u', uint32_t>();
  parser.add_argument("-i", "--inode-dir").default_value(std::string{"inodes"});
  parser.add_argument("-q", "--parquet-format").default_value(false).implicit_value(true);
  parser.add_argument("-a", "--all-inodes").default_value(false).implicit_value(true);
  parser.add_argument("-t", "--threads").default_value(uint32_t(4)).scan<'u', uint32_t>();
  return parser;
}

CoTryTask<Dispatcher::OutputTable> dumpInodes(IEnv &ienv,
                                              const argparse::ArgumentParser &parser,
                                              const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  ENSURE_USAGE(env.mgmtdClientGetter);
  ENSURE_USAGE(env.kvEngineGetter);

  const auto &numInodesPerFile = parser.get<uint32_t>("num-inodes-perfile");
  const auto &inodeDir = parser.get<std::string>("inode-dir");
  const auto &parquetFormat = parser.get<bool>("parquet-format");
  const auto &allInodes = parser.get<bool>("all-inodes");
  const auto &threads = parser.get<uint32_t>("threads");

  ENSURE_USAGE(threads > 0);

  if (boost::filesystem::exists(inodeDir)) {
    XLOGF(CRITICAL, "Output directory for inodes already exists: {}", inodeDir);
    co_return makeError(StatusCode::kInvalidArg);
  }

  Dispatcher::OutputTable table;
  auto dumpRes = co_await dumpInodesFromFdb(env.kvEngineGetter(),
                                            numInodesPerFile,
                                            inodeDir,
                                            parquetFormat,
                                            allInodes,
                                            std::max((uint32_t)1, threads));
  if (!dumpRes) co_return makeError(dumpRes.error());
  co_return table;
}

}  // namespace

CoTryTask<Void> dumpInodesFromFdb(std::shared_ptr<kv::IKVEngine> kvEngine,
                                  const uint32_t numInodesPerFile,
                                  const std::string inodeDir,
                                  const bool parquetFormat,
                                  const bool dumpAllInodes,
                                  const uint32_t threads) {
  boost::system::error_code err{};
  boost::filesystem::create_directories(inodeDir, err);

  if (UNLIKELY(err.failed())) {
    XLOGF(CRITICAL, "Failed to create directory {}, error: {}", inodeDir, err.message());
    co_return makeError(StatusCode::kIOError);
  }

  XLOGF(CRITICAL, "Saving inodes to directory: {}", inodeDir);

  meta::server::MetaScan::Options options;
  options.threads = 8;
  options.coroutines = 32;
  auto scan = std::make_unique<meta::server::MetaScan>(options, kvEngine);
  auto exec = std::make_unique<folly::CPUThreadPoolExecutor>(16);

  time_t timestamp = UtcClock::secondsSinceEpoch();
  InodeTable inodeBatch{.timestamp = timestamp};
  inodeBatch.inodes.reserve(numInodesPerFile);
  size_t numInodesSaved = 0;
  std::atomic<size_t> running = 0;

  auto dumpInodeTable = [&running, inodeDir, parquetFormat](size_t numInodesSaved,
                                                            const InodeTable inodeBatch) -> CoTask<bool> {
    SCOPE_EXIT { running--; };
    auto filePath = Path(inodeDir) / fmt::format("{}.inodes", numInodesSaved);
    bool writeOk = parquetFormat ? inodeBatch.dumpToParquetFile(filePath.replace_extension(".parquet"))
                                 : inodeBatch.dumpToFile(filePath);
    XLOGF_IF(WARN, writeOk, "{} inodes saved to file: {}", inodeBatch.inodes.size(), filePath);
    co_return writeOk;
  };

  std::vector<folly::SemiFuture<bool>> tasks;
  while (true) {
    auto inodes = scan->getInodes();

    for (const auto &inode : inodes) {
      if (inode.isFile() || dumpAllInodes) {
        inodeBatch.inodes.push_back({timestamp, inode});
        numInodesSaved++;
      }
    }

    bool fullBatch = inodeBatch.inodes.size() >= numInodesPerFile;
    bool lastBatch = inodes.empty() && !inodeBatch.inodes.empty();

    if (fullBatch || lastBatch) {
      running++;
      auto task = folly::coro::co_invoke(dumpInodeTable,
                                         numInodesSaved,
                                         std::exchange(inodeBatch, InodeTable{.timestamp = timestamp}))
                      .scheduleOn(exec.get())
                      .start();
      tasks.push_back(std::move(task));
      inodeBatch.inodes.reserve(numInodesPerFile);

      while (running >= threads) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }

    if (inodes.empty()) break;
  }

  auto result = co_await folly::coro::collectAllRange(std::move(tasks));
  auto succ = std::all_of(result.begin(), result.end(), [](auto v) { return v; });
  if (!succ) {
    co_return makeError(StatusCode::kIOError);
  }

  XLOGF(CRITICAL, "In total {} inodes saved to directory: {}", numInodesSaved, inodeDir);
  co_return Void{};
}

std::vector<Path> listFilesFromPath(const Path path) {
  std::vector<Path> filePaths;

  if (boost::filesystem::is_directory(path)) {
    for (boost::filesystem::directory_iterator iter{path}; iter != boost::filesystem::directory_iterator(); iter++) {
      if (boost::filesystem::is_regular_file(iter->path())) {
        filePaths.push_back(iter->path());
      }
    }
  } else {
    filePaths.push_back(path);
  }

  return filePaths;
}

CoTryTask<robin_hood::unordered_set<meta::InodeId>> loadInodeFromFiles(
    const Path inodePath,
    const robin_hood::unordered_set<meta::InodeId> &inodeIdsToPeek,
    const uint32_t parallel,
    const bool parquetFormat,
    std::time_t *inodeDumpTime) {
  std::mutex inodeIdsMutex;
  size_t inodeFilesLoaded = 0;
  robin_hood::unordered_set<meta::InodeId> uniqInodeIds(10'000'000);
  std::time_t inodeFileMinDumpTime(UtcClock::secondsSinceEpoch());

  auto executor = std::make_unique<folly::CPUThreadPoolExecutor>(parallel);
  std::vector<Path> inodeFilePaths = listFilesFromPath(inodePath);
  CountDownLatch<folly::fibers::Baton> loadInodesDone(inodeFilePaths.size());

  for (size_t inodeFileIndex = 0; inodeFileIndex < inodeFilePaths.size(); inodeFileIndex++) {
    executor->add([&, inodeFileIndex, inodeFilePath = inodeFilePaths[inodeFileIndex]]() {
      auto guard = folly::makeGuard([&loadInodesDone]() { loadInodesDone.countDown(); });
      InodeTable inodeBatch;

      bool ok = parquetFormat ? inodeBatch.loadFromParquetFile(inodeFilePath) : inodeBatch.loadFromFile(inodeFilePath);
      if (!ok) {
        XLOGF(FATAL, "Failed to load file: {}", inodeFilePath);
        return false;
      }

      std::vector<meta::InodeId> inodeIdVec;
      inodeIdVec.reserve(inodeBatch.inodes.size());

      for (const auto &inodeRow : inodeBatch.inodes) {
        inodeIdVec.push_back(inodeRow.inode.id);

        if (!inodeIdsToPeek.empty() && inodeIdsToPeek.count(inodeRow.inode.id)) {
          XLOGF(CRITICAL, "Found inode {}: {}", inodeRow.inode.id, inodeRow.inode);
        }
      }

      {
        std::scoped_lock lock(inodeIdsMutex);
        inodeFileMinDumpTime = std::min(inodeBatch.timestamp, inodeFileMinDumpTime);
        uniqInodeIds.insert(inodeIdVec.begin(), inodeIdVec.end());
        inodeFilesLoaded++;

        XLOGF(WARN,
              "#{}/{} Loaded {} inodes from file: {}, total number of inodes: {}, timestamp: {:%c}",
              inodeFileIndex + 1,
              inodeFilePaths.size(),
              inodeBatch.inodes.size(),
              inodeFilePath,
              uniqInodeIds.size(),
              fmt::localtime(inodeBatch.timestamp));
      }

      return true;
    });
  }

  co_await loadInodesDone.wait();
  executor->join();

  XLOGF(CRITICAL,
        "Loaded {} inodes from {}/{} files in path: {}",
        uniqInodeIds.size(),
        inodeFilesLoaded,
        inodeFilePaths.size(),
        inodePath);

  if (inodeFilesLoaded < inodeFilePaths.size()) {
    XLOGF(FATAL,
          "Failed to load {}/{} inode files in path: {}",
          inodeFilePaths.size() - inodeFilesLoaded,
          inodeFilePaths.size(),
          inodePath);
    co_return makeError(StatusCode::kIOError);
  }

  if (inodeDumpTime != nullptr) *inodeDumpTime = inodeFileMinDumpTime;
  co_return uniqInodeIds;
}

bool InodeTable::dumpToFile(const Path &filePath) const {
  std::ofstream dumpFile{filePath};
  if (UNLIKELY(!dumpFile)) {
    XLOGF(ERR, "Failed to create file: {}", filePath);
    return false;
  }

  auto bytes = serde::serializeBytes(*this);
  auto str = std::string_view{bytes};
  dumpFile.write(str.data(), str.size());

  if (UNLIKELY(!dumpFile)) {
    XLOGF(ERR, "Failed to write to file: {}", filePath);
    return false;
  }

  return true;
}

bool InodeTable::loadFromFile(const Path &filePath) {
  std::ifstream inputFile{filePath, std::ios_base::binary};
  if (UNLIKELY(!inputFile)) {
    XLOGF(ERR, "Failed to open file: {}", filePath);
    return false;
  }

  auto fileSize = static_cast<size_t>(boost::filesystem::file_size(filePath));
  std::string str;
  str.resize(fileSize, '\0');

  inputFile.read(&str[0], fileSize);

  if (str.size() != fileSize) {
    XLOGF(ERR, "Read size {} not equal to file size {}, file: {}", str.size(), fileSize, filePath);
    return false;
  }

  auto result = serde::deserialize(*this, str);

  return bool(result);
}

bool InodeTable::dumpToParquetFile(const Path &filePath) const {
  auto openRes = analytics::SerdeObjectWriter<InodeRow>::open(filePath);
  if (!openRes) return false;

  auto serdeWriter = std::move(*openRes);

  for (const auto &inode : inodes) {
    if (inode.inode.isDirectory()) {
      utf8makevalid((utf8_int8_t *)inode.inode.asDirectory().name.c_str(), '?');
    }
    serdeWriter << inode;
    if (!serdeWriter) break;
  }

  auto timestamp = UtcClock::secondsSinceEpoch();
  serdeWriter << InodeRow{timestamp, meta::Inode{}};

  return serdeWriter.ok();
}

bool InodeTable::loadFromParquetFile(const Path &filePath) {
  auto openRes = analytics::SerdeObjectReader<InodeRow>::open(filePath);
  if (!openRes) return false;

  auto serdeReader = std::move(*openRes);
  inodes.reserve(serdeReader.numRows());
  timestamp = UtcClock::secondsSinceEpoch();

  InodeRow inodeRow;
  while (serdeReader >> inodeRow) {
    inodes.push_back(inodeRow);
    timestamp = std::min(timestamp, inodeRow.timestamp);
  }

  ERRLOGF_IF(INFO,
             inodes.size() != serdeReader.numRows(),
             "Loaded {} inodes from {} rows in file: {}",
             inodes.size(),
             serdeReader.numRows(),
             filePath);
  return serdeReader.ok();
}

CoTryTask<void> registerDumpInodesHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, dumpInodes);
}

}  // namespace hf3fs::client::cli
