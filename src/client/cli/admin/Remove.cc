#include "Remove.h"

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("rm");
  parser.add_argument("path");
  parser.add_argument("-r", "--recursive").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleRemove(IEnv &ienv,
                                                const argparse::ArgumentParser &parser,
                                                const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto path = parser.get<std::string>("path");
  auto recursive = parser.get<bool>("-r");
  auto res = co_await env.metaClientGetter()->remove(env.userInfo, env.currentDirId, Path(path), recursive);
  CO_RETURN_ON_ERROR(res);
  co_return table;
}

}  // namespace

CoTryTask<void> registerRemoveHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleRemove);
}
}  // namespace hf3fs::client::cli
