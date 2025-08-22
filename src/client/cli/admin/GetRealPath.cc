#include "GetRealPath.h"

#include <fmt/chrono.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/MagicEnum.hpp"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("getrealpath");
  parser.add_argument("path");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleGetRealPath(IEnv &ienv,
                                                     const argparse::ArgumentParser &parser,
                                                     const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;
  auto path = parser.present<std::string>("path").value_or(env.currentDir);
  auto res = co_await env.metaClientGetter()->getRealPath(env.userInfo, env.currentDirId, Path(path), true);
  CO_RETURN_ON_ERROR(res);
  table.push_back({res.value().native()});
  co_return table;
}
}  // namespace

CoTryTask<void> registerGetRealPathHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleGetRealPath);
}
}  // namespace hf3fs::client::cli
