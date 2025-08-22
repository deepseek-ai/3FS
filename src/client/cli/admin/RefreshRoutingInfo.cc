#include "RefreshRoutingInfo.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("refresh-routing-info");
  parser.add_argument("-f", "--force").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto force = parser.get<bool>("-f");

  CO_RETURN_ON_ERROR(co_await env.unsafeMgmtdClientGetter()->refreshRoutingInfo(force));

  co_return table;
}
}  // namespace

CoTryTask<void> registerRefreshRoutingInfoHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
