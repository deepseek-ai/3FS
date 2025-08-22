#include "SetPreferredTargetOrder.h"

#include <scn/scn.h>

#include "AdminEnv.h"
#include "Utils.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/Transform.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("set-preferred-target-order");
  parser.add_argument("chainId").scan<'u', uint32_t>();
  parser.add_argument("targetIds").scan<'u', uint64_t>().remaining();
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto cid = flat::ChainId(parser.get<uint32_t>("chainId"));
  auto targetIds = parser.get<std::vector<uint64_t>>("targetIds");
  auto tids = transformTo<std::vector>(std::span{targetIds.begin(), targetIds.size()},
                                       [](uint64_t id) { return flat::TargetId(id); });
  auto res = co_await env.mgmtdClientGetter()->setPreferredTargetOrder(env.userInfo, cid, tids);
  CO_RETURN_ON_ERROR(res);

  statChainInfo(table, *res);
  co_return table;
}

}  // namespace

CoTryTask<void> registerSetPreferredTargetOrderHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
