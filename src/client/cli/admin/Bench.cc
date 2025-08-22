#include "client/cli/admin/Bench.h"

#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <folly/Random.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/Invoke.h>
#include <fstream>
#include <sstream>

#include "AdminEnv.h"
#include "client/cli/admin/FileWrapper.h"
#include "client/cli/admin/Layout.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "client/storage/StorageClient.h"
#include "client/storage/TargetSelection.h"
#include "common/serde/Serde.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "fbs/storage/Common.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("bench");
  parser.add_argument("path");
  parser.add_argument("--rank").default_value(uint32_t{0}).scan<'u', uint32_t>();
  parser.add_argument("--timeout").default_value(uint32_t{1}).scan<'u', uint32_t>();
  parser.add_argument("--coroutines").default_value(uint32_t{1}).scan<'u', uint32_t>();
  parser.add_argument("--seconds").default_value(uint32_t{60}).scan<'u', uint32_t>();
  parser.add_argument("--remove").default_value(false).implicit_value(true);
  return parser;
}

Result<Void> check(const Path &src, uint32_t expected, storage::ChecksumInfo checksum) {
  if (checksum.value != expected) {
    auto msg = fmt::format("check {} failed {:08x} != {:08x}", src, checksum.value, expected);
    XLOG(ERR, msg);
    return makeError(StorageClientCode::kChecksumMismatch, std::move(msg));
  }
  return Void{};
}

CoTryTask<Void> writeFileAndCheck(AdminEnv &env,
                                  const Path &path,
                                  std::string_view str,
                                  uint32_t repeat,
                                  Duration timeout,
                                  uint32_t crc32c) {
  auto openResult = co_await FileWrapper::openOrCreateFile(env, path, true);
  CO_RETURN_AND_LOG_ON_ERROR(openResult);
  auto &file = *openResult;

  boost::iostreams::stream<boost::iostreams::basic_array_source<char>> stream(str.begin(), str.size());
  auto writeResult = co_await file.writeFile(stream, str.size(), 0, 10_s);
  CO_RETURN_AND_LOG_ON_ERROR(writeResult);
  CO_RETURN_AND_LOG_ON_ERROR(check(path, crc32c, *writeResult));

  std::ofstream out("/dev/null");
  auto readResult =
      co_await file.readFile(out, str.size(), 0, true, false, storage::client::TargetSelectionMode::HeadTarget);
  CO_RETURN_AND_LOG_ON_ERROR(readResult);
  CO_RETURN_AND_LOG_ON_ERROR(check(path, crc32c, *readResult));

  readResult =
      co_await file.readFile(out, str.size(), 0, true, false, storage::client::TargetSelectionMode::TailTarget);
  CO_RETURN_AND_LOG_ON_ERROR(readResult);
  CO_RETURN_AND_LOG_ON_ERROR(check(path, crc32c, *readResult));
  co_return Void{};
}

CoTryTask<Dispatcher::OutputTable> handleWriteFile(IEnv &ienv,
                                                   const argparse::ArgumentParser &parser,
                                                   const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  // 1. parse arguments.
  Path src = parser.get<std::string>("path");
  auto rank = parser.get<uint32_t>("--rank");
  auto timeout = 1_s * parser.get<uint32_t>("--timeout");
  auto coroutines = parser.get<uint32_t>("--coroutines");
  auto deadline = RelativeTime::now() + 1_s * parser.get<uint32_t>("--seconds");
  auto remove = parser.get<bool>("--remove");

  // 2. prepare datas.
  std::vector<std::string> datas;
  std::vector<uint32_t> crc32cList;
  for (auto i = 0; i < 16; ++i) {
    datas.emplace_back(std::string(128_MB, 'A' + i));
    auto checksum = storage::ChecksumInfo::create(storage::ChecksumType::CRC32C,
                                                  reinterpret_cast<const uint8_t *>(datas.back().data()),
                                                  datas.back().length());
    crc32cList.emplace_back(checksum.value);
  }

  // 3. write and check.
  std::vector<CoTryTask<Void>> total;
  total.reserve(coroutines);
  for (auto i = 0u; i < coroutines; ++i) {
    total.push_back(folly::coro::co_invoke([&, i]() -> CoTryTask<Void> {
      auto path = src / fmt::format("{:03}.{:03}", rank, i);
      while (RelativeTime::now() <= deadline) {
        auto idx = folly::Random::rand32() % datas.size();
        CO_RETURN_AND_LOG_ON_ERROR(co_await writeFileAndCheck(env, path, datas[idx], 1, timeout, crc32cList[idx])
                                       .scheduleOn(folly::getGlobalCPUExecutor()));
        if (remove) {
          CO_RETURN_AND_LOG_ON_ERROR(
              co_await env.metaClientGetter()->remove(env.userInfo, env.currentDirId, path, false));
        }
      }
      co_return Void{};
    }));
  }

  auto results = co_await folly::coro::collectAllRange(std::move(total));
  for (auto result : results) {
    CO_RETURN_AND_LOG_ON_ERROR(result);
  }

  co_return table;
}

}  // namespace

CoTryTask<void> registerBenchHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleWriteFile);
}

}  // namespace hf3fs::client::cli
