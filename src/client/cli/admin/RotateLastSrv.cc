#include "RotateLastSrv.h"

#include <scn/scn.h>

#include "AdminEnv.h"
#include "Utils.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("rotate-lastsrv");
  parser.add_argument("chainId").scan<'u', uint32_t>();
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto cid = flat::ChainId(parser.get<uint32_t>("chainId"));
  auto res = co_await env.mgmtdClientGetter()->rotateLastSrv(env.userInfo, cid);
  CO_RETURN_ON_ERROR(res);

  statChainInfo(table, *res);
  co_return table;
}

}  // namespace

CoTryTask<void> registerRotateLastSrvHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
