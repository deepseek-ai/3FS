#include "AdminEnv.h"
#include "Mkdir.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("ln");
  parser.add_argument("target");
  parser.add_argument("linkname");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleSymlink(IEnv &ienv,
                                                 const argparse::ArgumentParser &parser,
                                                 const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto target = parser.get<std::string>("target");
  auto linkname = parser.get<std::string>("linkname");
  auto res = co_await env.metaClientGetter()->symlink(env.userInfo, env.currentDirId, Path(linkname), Path(target));
  CO_RETURN_ON_ERROR(res);
  co_return table;
}

}  // namespace

CoTryTask<void> registerSymlinkHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleSymlink);
}
}  // namespace hf3fs::client::cli
