#include "Rename.h"

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("mv");
  parser.add_argument("src");
  parser.add_argument("dst");
  return parser;
}
CoTryTask<Dispatcher::OutputTable> handleRename(IEnv &ienv,
                                                const argparse::ArgumentParser &parser,
                                                const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  Path src{parser.get<std::string>("src")};
  Path dst{parser.get<std::string>("dst")};
  auto res = co_await env.metaClientGetter()->rename(env.userInfo, env.currentDirId, src, env.currentDirId, dst);
  CO_RETURN_ON_ERROR(res);
  co_return table;
}

}  // namespace

CoTryTask<void> registerRenameHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleRename);
}
}  // namespace hf3fs::client::cli
