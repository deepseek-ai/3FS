#include "SetLayout.h"

#include "AdminEnv.h"
#include "client/cli/admin/Layout.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("set-layout");
  parser.add_argument("path");
  addLayoutArguments(parser);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleSetLayout(IEnv &ienv,
                                                   const argparse::ArgumentParser &parser,
                                                   const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto path = parser.get<std::string>("path");
  auto res = co_await env.metaClientGetter()->stat(env.userInfo, env.currentDirId, Path(path), true);
  CO_RETURN_ON_ERROR(res);
  auto &inode = *res;
  if (!inode.isDirectory()) {
    co_return makeError(MetaCode::kNotDirectory, "SetLayout only support on directory");
  }
  auto layout = parseLayout(parser, inode.asDirectory().layout);
  ENSURE_USAGE(layout.has_value());
  CO_RETURN_ON_ERROR(co_await env.metaClientGetter()->setLayout(env.userInfo, inode.id, std::nullopt, *layout));
  co_return table;
}

}  // namespace

CoTryTask<void> registerSetLayoutHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleSetLayout);
}
}  // namespace hf3fs::client::cli
