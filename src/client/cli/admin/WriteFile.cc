#include "WriteFile.h"

#include <folly/experimental/coro/BlockingWait.h>
#include <fstream>
#include <sstream>

#include "AdminEnv.h"
#include "client/cli/admin/FileWrapper.h"
#include "client/cli/admin/Layout.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "client/storage/StorageClient.h"
#include "common/serde/Serde.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "fbs/storage/Common.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("write-file");
  parser.add_argument("path");
  parser.add_argument("--stat").default_value(false).implicit_value(true);
  parser.add_argument("--offset");
  parser.add_argument("--length");
  parser.add_argument("--timeout").default_value(uint32_t{1}).scan<'u', uint32_t>();
  parser.add_argument("--notTruncate").default_value(false).implicit_value(true);
  parser.add_argument("-i", "--input");
  parser.add_argument("--value").scan<'u', uint8_t>();
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleWriteFile(IEnv &ienv,
                                                   const argparse::ArgumentParser &parser,
                                                   const Dispatcher::Args &args) {
  [[maybe_unused]] auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  // 1. parse arguments.
  Path src = parser.get<std::string>("path");
  auto stat = parser.get<bool>("--stat");
  auto input = parser.present<std::string>("--input");
  auto timeout = 1_s * parser.get<uint32_t>("--timeout");
  auto doTruncate = !parser.get<bool>("--notTruncate");
  size_t offset = 0;
  if (auto o = parser.present("--offset")) {
    auto offsetResult = Size::from(*o);
    CO_RETURN_AND_LOG_ON_ERROR(offsetResult);
    offset = *offsetResult;
  }
  std::optional<size_t> length;
  if (auto l = parser.present<std::string>("--length")) {
    auto lengthResult = Size::from(*l);
    CO_RETURN_AND_LOG_ON_ERROR(lengthResult);
    length = *lengthResult;
  }
  auto value = parser.present<uint8_t>("--value");

  // 2. prepare input data.
  std::ifstream inputFile;
  std::string inputString;
  uint64_t inputLength = 0;
  if (input.has_value()) {
    inputFile = std::ifstream{input.value()};
    if (!inputFile) {
      co_return makeError(StatusCode::kInvalidArg, fmt::format("open input file {} failed", input.value()));
    }
    inputLength = boost::filesystem::file_size(input.value());
  } else if (value.has_value()) {
    inputString = std::string(128_MB, char(*value));
    inputLength = inputString.length();
  } else {
    std::string line;
    while (std::getline(std::cin, line)) {
      inputString += line + '\n';
    }
    if (inputString.empty()) {
      co_return makeError(StatusCode::kInvalidArg, fmt::format("get line from stdin failed"));
    }
    inputLength = inputString.length();
  }

  // 3. stat file.
  auto openResult = co_await FileWrapper::openOrCreateFile(env, src, true);
  CO_RETURN_AND_LOG_ON_ERROR(openResult);
  auto &file = *openResult;

  // 4. calc chain and chunk id.
  auto writeLength = length.has_value() ? *length : inputLength;
  if (file.length() > offset + writeLength && doTruncate) {
    auto result = co_await file.truncate(offset + writeLength);
    CO_RETURN_AND_LOG_ON_ERROR(result);
  }
  if (stat) {
    file.showChunks(offset, writeLength);
  }

  // 5. write file.
  storage::ChecksumInfo checksum{};
  auto repeatTimes = (writeLength + inputLength - 1) / inputLength;
  for (auto i = 0ul; i < repeatTimes; ++i) {
    std::unique_ptr<std::istream> in;
    if (input.has_value()) {
      in = std::make_unique<std::ifstream>(input.value());
      if (UNLIKELY(!*in)) {
        co_return makeError(StatusCode::kInvalidArg, fmt::format("open input file {} failed", input.value()));
      }
    } else {
      in = std::make_unique<std::istringstream>(inputString);
    }
    auto w = std::min(inputLength, writeLength - inputLength * i);
    auto writeResult = co_await file.writeFile(*in, w, offset + inputLength * i, timeout, false);
    CO_RETURN_AND_LOG_ON_ERROR(writeResult);
    checksum.combine(*writeResult, w);
  }

  CO_RETURN_AND_LOG_ON_ERROR(co_await file.sync());
  std::cout << fmt::format("write length {}, checksum {}", writeLength, checksum) << std::endl;
  co_return table;
}

}  // namespace

CoTryTask<void> registerWriteFileHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleWriteFile);
}

}  // namespace hf3fs::client::cli
