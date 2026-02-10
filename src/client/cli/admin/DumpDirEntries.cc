#include "DumpDirEntries.h"

#include <algorithm>
#include <cstdint>
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>
#include <thread>
#include <vector>

#include "AdminEnv.h"
#include "analytics/SerdeObjectReader.h"
#include "analytics/SerdeObjectWriter.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/IEnv.h"
#include "client/cli/common/Utils.h"
#include "common/logging/LogHelper.h"
#include "common/utils/Utf8.h"
#include "fbs/meta/Schema.h"
#include "fmt/core.h"
#include "meta/event/Scan.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("dump-dentries");
  parser.add_argument("-n", "--num-dentries-perfile").default_value(uint32_t{10'000'000}).scan<'u', uint32_t>();
  parser.add_argument("-d", "--dentry-dir").default_value(std::string{"dentries"});
  parser.add_argument("-t", "--threads").default_value(uint32_t(4)).scan<'u', uint32_t>();
  return parser;
}

CoTryTask<Dispatcher::OutputTable> dumpDirEntries(IEnv &ienv,
                                                  const argparse::ArgumentParser &parser,
                                                  const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  ENSURE_USAGE(env.mgmtdClientGetter);
  ENSURE_USAGE(env.kvEngineGetter);

  const auto &numEntriesPerFile = parser.get<uint32_t>("num-dentries-perfile");
  const auto &dentryDir = parser.get<std::string>("dentry-dir");
  const auto threads = parser.get<uint32_t>("threads");

  ENSURE_USAGE(threads > 0);
  ENSURE_USAGE(!dentryDir.empty());
  ENSURE_USAGE(numEntriesPerFile > 0);

  if (boost::filesystem::exists(dentryDir)) {
    XLOGF(CRITICAL, "Output directory for directory entries already exists: {}", dentryDir);
    co_return makeError(StatusCode::kInvalidArg, "output directory already exists");
  }

  Dispatcher::OutputTable table;
  auto dumpRes =
      co_await dumpDirEntriesFromFdb(env.kvEngineGetter(), numEntriesPerFile, dentryDir, std::max(uint32_t(1), threads));
  if (!dumpRes) co_return makeError(dumpRes.error());
  co_return table;
}
}  // namespace

CoTryTask<Void> dumpDirEntriesFromFdb(std::shared_ptr<kv::IKVEngine> kvEngine,
                                      const uint32_t numEntriesPerDir,
                                      const std::string dentryDir,
                                      const uint32_t threads) {
  boost::system::error_code err{};
  boost::filesystem::create_directories(dentryDir, err);

  if (UNLIKELY(err.failed())) {
    XLOGF(CRITICAL, "Failed to create directory {}, error: {}", dentryDir, err.message());
    co_return makeError(StatusCode::kIOError,
                        fmt::format("failed to create directory {}, error {}", dentryDir, err.message()));
  }

  XLOGF(CRITICAL, "Saving directory entries to directory: {}", dentryDir);

  meta::server::MetaScan::Options options;
  options.threads = 8;
  options.coroutines = 32;
  auto scan = std::make_unique<meta::server::MetaScan>(options, kvEngine);
  auto exec = std::make_unique<folly::CPUThreadPoolExecutor>(16);

  time_t timestamp = UtcClock::secondsSinceEpoch();
  DirEntryTable dentryBatch{.timestamp = timestamp};
  dentryBatch.entries.reserve(numEntriesPerDir);
  size_t numEntriesSaved = 0;

  std::atomic<size_t> running = 0;

  auto dumpDirEntryTable = [&running, dentryDir](size_t numEntriesSaved,
                                                 const DirEntryTable dentryBatch) -> CoTask<bool> {
    SCOPE_EXIT { running--; };
    auto filePath = Path(dentryDir) / fmt::format("{}.parquet", numEntriesSaved);
    bool writeOk = dentryBatch.dumpToParquetFile(filePath);
    XLOGF_IF(WARN, writeOk, "{} directory entries saved to file: {}", dentryBatch.entries.size(), filePath);
    co_return writeOk;
  };

  std::vector<folly::SemiFuture<bool>> tasks;
  while (true) {
    auto entries = scan->getDirEntries();

    for (auto &entry : entries) {
      dentryBatch.entries.push_back({timestamp, entry});
      numEntriesSaved++;
    }

    bool fullBatch = dentryBatch.entries.size() >= numEntriesPerDir;
    bool lastBatch = entries.empty() && !dentryBatch.entries.empty();

    if (fullBatch || lastBatch) {
      running++;
      auto task = folly::coro::co_invoke(dumpDirEntryTable,
                                         numEntriesSaved,
                                         std::exchange(dentryBatch, DirEntryTable{.timestamp = timestamp}))
                      .scheduleOn(exec.get())
                      .start();
      tasks.push_back(std::move(task));
      dentryBatch.entries.reserve(numEntriesPerDir);

      while (running >= threads) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    if (entries.empty()) break;
  }

  auto result = co_await folly::coro::collectAllRange(std::move(tasks));
  auto succ = std::all_of(result.begin(), result.end(), [](auto v) { return v; });
  if (!succ) {
    co_return makeError(StatusCode::kIOError);
  }

  XLOGF(CRITICAL, "In total {} directory entries saved to directory: {}", numEntriesSaved, dentryDir);
  co_return Void{};
}

bool DirEntryTable::dumpToParquetFile(const Path &filePath) const {
  auto openRes = analytics::SerdeObjectWriter<DirEntryRow>::open(filePath);
  if (!openRes) return false;

  auto serdeWriter = std::move(*openRes);

  for (const auto &entry : entries) {
    utf8makevalid((utf8_int8_t *)entry.entry.name.c_str(), '?');
    serdeWriter << entry;
    if (!serdeWriter) break;
  }

  auto timestamp = UtcClock::secondsSinceEpoch();
  serdeWriter << DirEntryRow{timestamp, meta::DirEntry{}};

  return serdeWriter.ok();
}

CoTryTask<void> registerDumpDirEntriesHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, dumpDirEntries);
}

}  // namespace hf3fs::client::cli