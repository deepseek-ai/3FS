#include "ReadFile.h"

#include <folly/experimental/coro/BlockingWait.h>
#include <fstream>
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/md5.h>

#include "AdminEnv.h"
#include "client/cli/admin/FileWrapper.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "client/storage/StorageClient.h"
#include "client/storage/TargetSelection.h"
#include "common/serde/Serde.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "fbs/storage/Common.h"
#include "scn/scan/scan.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("read-file");
  parser.add_argument("path");
  parser.add_argument("--stat").default_value(false).implicit_value(true);
  parser.add_argument("--offset");
  parser.add_argument("--length");
  parser.add_argument("--mode");
  parser.add_argument("--disableChecksum").default_value(false).implicit_value(true);
  parser.add_argument("--fillZero").default_value(false).implicit_value(true);
  parser.add_argument("--verbose").default_value(false).implicit_value(true);
  parser.add_argument("-o", "--output");
  parser.add_argument("--hex").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleReadFile(IEnv &ienv,
                                                  const argparse::ArgumentParser &parser,
                                                  const Dispatcher::Args &args) {
  [[maybe_unused]] auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  // 1. parse arguments.
  Path src = parser.get<std::string>("path");
  auto stat = parser.get<bool>("--stat");
  auto enableChecksum = !parser.get<bool>("--disableChecksum");
  auto fillZero = parser.get<bool>("--fillZero");
  auto verbose = parser.get<bool>("--verbose");
  auto output = parser.present<std::string>("--output");
  auto hex = parser.get<bool>("--hex");

  size_t offset = 0;
  if (auto o = parser.present("--offset")) {
    auto offsetResult = Size::from(*o);
    CO_RETURN_AND_LOG_ON_ERROR(offsetResult);
    offset = *offsetResult;
  }
  std::optional<size_t> lengthIn;
  if (auto l = parser.present<std::string>("--length")) {
    auto lengthResult = Size::from(*l);
    CO_RETURN_AND_LOG_ON_ERROR(lengthResult);
    lengthIn = *lengthResult;
  }
  storage::client::TargetSelectionMode mode = storage::client::TargetSelectionMode::Default;
  uint32_t targetIndex = 0;
  if (auto m = parser.present("--mode")) {
    auto result = magic_enum::enum_cast<storage::client::TargetSelectionMode>(*m);
    if (result) {
      mode = *result;
    } else if (auto idx = scn::scan_value<uint32_t>(*m)) {
      mode = storage::client::TargetSelectionMode::ManualMode;
      targetIndex = idx.value();
    } else {
      co_return makeError(StatusCode::kInvalidArg, fmt::format("invalid mode", *m));
    }
  }

  // 2. refresh routing info.
  CO_RETURN_AND_LOG_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(true));
  auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo()->raw();
  if (routingInfo == nullptr) {
    co_return makeError(StorageClientCode::kRoutingError, "routing info is null");
  }

  // 3. open file.
  auto openResult = co_await FileWrapper::openOrCreateFile(env, src);
  CO_RETURN_AND_LOG_ON_ERROR(openResult);
  auto &file = *openResult;

  // 4. calc chain and chunk id.
  if (file.length() <= offset) {
    co_return makeError(StatusCode::kInvalidArg, fmt::format("offset {} exceed file length {}", offset, file.length()));
  }
  auto readLength = std::min(lengthIn.value_or(file.length()), file.length() - offset);
  if (stat) {
    file.showChunks(offset, readLength);
  }

  // 5. read file.
  std::ofstream outputFile;
  if (output.has_value()) {
    outputFile = std::ofstream{output.value()};
    if (!outputFile) {
      co_return makeError(StatusCode::kInvalidArg, fmt::format("create output file {} failed", output.value()));
    }
  }
  std::ostream &out = output.has_value() ? outputFile : std::cout;

  MD5_CTX md5;
  int md5ret = MD5_Init(&md5);
  if (UNLIKELY(md5ret != 1)) {
    co_return makeError(StatusCode::kInvalidArg, fmt::format("md5 init failed: {}", md5ret));
  }
  auto writeResult =
      co_await file.readFile(out, readLength, offset, enableChecksum, hex, mode, &md5, fillZero, verbose, targetIndex);
  CO_RETURN_AND_LOG_ON_ERROR(writeResult);
  auto checksum = *writeResult;
  std::array<uint8_t, MD5_DIGEST_LENGTH> md5out{};
  md5ret = MD5_Final(md5out.data(), &md5);
  if (UNLIKELY(md5ret != 1)) {
    co_return makeError(StatusCode::kInvalidArg, fmt::format("md5 final failed: {}", md5ret));
  }
  std::cout << fmt::format("read length {}, checksum {}, md5 {:02x}\n", readLength, checksum, fmt::join(md5out, ""));
  co_return table;
}

}  // namespace

CoTryTask<void> registerReadFileHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleReadFile);
}

}  // namespace hf3fs::client::cli
