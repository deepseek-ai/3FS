#include "Mkdir.h"

#include <scn/scn.h>

#include "AdminEnv.h"
#include "client/cli/admin/Layout.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("mkdir");
  parser.add_argument("path");
  parser.add_argument("-p", "-r", "--recursive").default_value(false).implicit_value(true);
  parser.add_argument("--perm");
  addLayoutArguments(parser);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleMkdir(IEnv &ienv,
                                               const argparse::ArgumentParser &parser,
                                               const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto path = parser.get<std::string>("path");
  auto recursive = parser.get<bool>("-r");
  auto p = parser.present<String>("--perm");

  meta::Permission perm(0755);
  if (p) {
    auto [r, v] = scn::scan_tuple<uint32_t>(*p, "{:o}");
    if (!r) co_return makeError(StatusCode::kInvalidArg, fmt::format("invalid permission format: {}", r.error().msg()));
    perm = meta::Permission(v);
  }

  auto layout = parseLayout(parser);
  auto res =
      co_await env.metaClientGetter()->mkdirs(env.userInfo, env.currentDirId, Path(path), perm, recursive, layout);
  CO_RETURN_ON_ERROR(res);
  co_return table;
}

}  // namespace

CoTryTask<void> registerMkdirHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleMkdir);
}
}  // namespace hf3fs::client::cli
