#include "Checksum.h"

#include <folly/executors/GlobalExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <fstream>
#include <memory>
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/md5.h>
#include <string>

#include "AdminEnv.h"
#include "client/cli/admin/FileWrapper.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "client/storage/StorageClient.h"
#include "common/serde/Serde.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "fbs/storage/Common.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("checksum");
  parser.add_argument("path").nargs(argparse::nargs_pattern::at_least_one);
  parser.add_argument("--list").default_value(false).implicit_value(true);
  parser.add_argument("--batch").default_value(uint32_t{8}).scan<'u', uint32_t>();
  parser.add_argument("--md5").default_value(false).implicit_value(true);
  parser.add_argument("--fillZero").default_value(false).implicit_value(true);
  parser.add_argument("-o", "--output");
  return parser;
}

struct ReadResult {
  Path path;
  Result<Void> result = makeError(Status::OK);
  storage::ChecksumInfo checksum;
  std::array<uint8_t, MD5_DIGEST_LENGTH> md5{};
};

CoTryTask<Void> checksumFile(AdminEnv &env, const Path &path, bool fillZero, bool md5Enabled, ReadResult &result) {
  result.path = path;

  auto openResult = co_await FileWrapper::openOrCreateFile(env, path, false);
  CO_RETURN_AND_LOG_ON_ERROR(openResult);
  auto &file = *openResult;

  MD5_CTX md5;
  int md5ret = MD5_Init(&md5);
  if (UNLIKELY(md5ret != 1)) {
    co_return makeError(StatusCode::kInvalidArg, fmt::format("md5 init failed: {}", md5ret));
  }

  auto replicasNum = file.replicasNum();
  CO_RETURN_AND_LOG_ON_ERROR(replicasNum);
  for (auto i = 0u; i < *replicasNum; ++i) {
    std::ofstream out("/dev/null");
    auto readResult = co_await file.readFile(out,
                                             file.length(),
                                             0,
                                             true,
                                             false,
                                             storage::client::TargetSelectionMode::ManualMode,
                                             i == 0 && md5Enabled ? &md5 : nullptr,
                                             fillZero,
                                             false,
                                             0);
    CO_RETURN_AND_LOG_ON_ERROR(readResult);
    if (i == 0) {
      result.checksum = *readResult;
    } else if (result.checksum != *readResult) {
      co_return makeError(
          StorageCode::kChecksumMismatch,
          fmt::format("file checksum mismatch, path {}, head {}, now:{} {}", path, result.checksum, i, *readResult));
    }
  }

  md5ret = MD5_Final(result.md5.data(), &md5);
  if (UNLIKELY(md5ret != 1)) {
    co_return makeError(StatusCode::kInvalidArg, fmt::format("md5 final failed: {}", md5ret));
  }

  co_return Void{};
}

CoTask<void> callChecksumFile(AdminEnv &env, const Path &path, bool fillZero, bool md5Enabled, ReadResult &result) {
  result.result =
      co_await checksumFile(env, path, fillZero, md5Enabled, result).scheduleOn(folly::getGlobalCPUExecutor());
}

CoTryTask<Dispatcher::OutputTable> handleChecksum(IEnv &ienv,
                                                  const argparse::ArgumentParser &parser,
                                                  const Dispatcher::Args &args) {
  [[maybe_unused]] auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  // 1. parse arguments.
  auto paths = parser.get<std::vector<std::string>>("path");
  auto list = parser.get<bool>("--list");
  auto batchSize = parser.get<uint32_t>("--batch");
  auto md5Enabled = parser.get<bool>("--md5");
  auto fillZero = parser.get<bool>("--fillZero");
  auto output = parser.present<std::string>("--output");

  // 2. prepare file list.
  std::vector<Path> fileList;
  for (auto &src : paths) {
    if (list) {
      std::ifstream listFile(src);
      if (!listFile) {
        co_return makeError(StatusCode::kInvalidArg, fmt::format("open list file failed, {}", src));
      }
      std::string line;
      while (std::getline(listFile, line)) {
        if (!line.empty()) {
          fileList.push_back(Path{line});
        }
      }
    } else {
      fileList.push_back(src);
    }
  }

  // 3. read file.
  std::ofstream outputFile;
  if (output.has_value()) {
    outputFile = std::ofstream{output.value()};
    if (!outputFile) {
      co_return makeError(StatusCode::kInvalidArg, fmt::format("create output file {} failed", output.value()));
    }
  }
  std::ostream &out = output.has_value() ? outputFile : std::cout;

  std::vector<ReadResult> readResults;
  readResults.reserve(batchSize);
  std::vector<CoTask<void>> batch;
  for (auto &filePath : fileList) {
    readResults.emplace_back();
    batch.push_back(callChecksumFile(env, filePath, fillZero, md5Enabled, readResults.back()));
    if (batch.size() >= batchSize || std::addressof(filePath) == std::addressof(fileList.back())) {
      co_await folly::coro::collectAllRange(std::move(batch));
      for (auto &result : readResults) {
        if (result.result.hasValue()) {
          if (md5Enabled) {
            out << fmt::format("{} {:02x}\n", result.path, fmt::join(result.md5, ""));
          } else {
            out << fmt::format("{} {:08X}\n", result.path, result.checksum.value);
          }
        } else {
          out << fmt::format("{} {}\n", result.path, result.result.error());
        }
      }
      readResults.clear();
    }
  }

  co_return table;
}

}  // namespace

CoTryTask<void> registerChecksumHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleChecksum);
}

}  // namespace hf3fs::client::cli
