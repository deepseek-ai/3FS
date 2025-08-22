#include "ParseTargetMeta.h"

#include <folly/experimental/coro/BlockingWait.h>
#include <fstream>

#include "AdminEnv.h"
#include "client/cli/admin/FileWrapper.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "client/storage/StorageClient.h"
#include "common/serde/Serde.h"
#include "common/utils/FileUtils.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "fbs/storage/Common.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("parse-target-meta");
  parser.add_argument("path");
  parser.add_argument("-o", "--output");
  parser.add_argument("--format").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleParseTargetMeta(IEnv &ienv,
                                                         const argparse::ArgumentParser &parser,
                                                         const Dispatcher::Args &args) {
  [[maybe_unused]] auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  // 1. parse arguments.
  Path src = parser.get<std::string>("path");
  auto output = parser.present<std::string>("--output");
  auto format = parser.get<bool>("--format");

  auto readResult = loadFile(src);
  CO_RETURN_AND_LOG_ON_ERROR(readResult);

  std::map<storage::ChunkId, storage::ChunkMetadata> metas;
  CO_RETURN_AND_LOG_ON_ERROR(serde::deserialize(metas, *readResult));

  // 5. write to file.
  std::ofstream outputFile;
  if (output.has_value()) {
    outputFile = std::ofstream{output.value()};
    if (!outputFile) {
      co_return makeError(StatusCode::kInvalidArg, fmt::format("create output file {} failed", output.value()));
    }
  }
  std::ostream &out = output.has_value() ? outputFile : std::cout;
  out << serde::toJsonString(metas, false, format) << std::endl;

  co_return table;
}

}  // namespace

CoTryTask<void> registerParseTargetMetaHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleParseTargetMeta);
}

}  // namespace hf3fs::client::cli
