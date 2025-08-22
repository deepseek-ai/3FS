#include "Create.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/admin/Layout.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "fbs/meta/Schema.h"
#include "fbs/mgmtd/ChainRef.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("create");
  parser.add_argument("-p", "--perm");
  parser.add_argument("path");
  addLayoutArguments(parser);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleCreate(IEnv &ienv,
                                                const argparse::ArgumentParser &parser,
                                                const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto path = parser.get<std::string>("path");
  auto layout = parseLayout(parser);
  auto p = parser.present<String>("-p");

  meta::Permission perm(0644);
  if (p) {
    auto [r, v] = scn::scan_tuple<uint32_t>(*p, "{:o}");
    if (!r) co_return makeError(StatusCode::kInvalidArg, fmt::format("invalid permission format: {}", r.error().msg()));
    perm = meta::Permission(v);
  }

  auto res = co_await env.metaClientGetter()
                 ->create(env.userInfo, env.currentDirId, Path(path), std::nullopt, perm, 0, layout);
  CO_RETURN_ON_ERROR(res);
  co_return table;
}

}  // namespace

CoTryTask<void> registerCreateHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleCreate);
}
}  // namespace hf3fs::client::cli
