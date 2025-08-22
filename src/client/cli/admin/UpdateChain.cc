#include "UpdateChain.h"

#include <boost/algorithm/string.hpp>
#include <scn/scn.h>

#include "AdminEnv.h"
#include "Utils.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("update-chain");
  parser.add_argument("chainId").scan<'u', uint32_t>();
  parser.add_argument("targetId").scan<'u', uint64_t>();
  parser.add_argument("-m", "--mode").help("add | remove").required();
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto cid = flat::ChainId(parser.get<uint32_t>("chainId"));
  auto tid = flat::TargetId(parser.get<uint64_t>("targetId"));
  auto modeStr = parser.get<String>("-m");
  auto lowerModeStr = boost::to_lower_copy(modeStr);
  auto mode = [&] {
    if (lowerModeStr == "add")
      return mgmtd::UpdateChainReq::Mode::ADD;
    else if (lowerModeStr == "remove")
      return mgmtd::UpdateChainReq::Mode::REMOVE;
    ENSURE_USAGE(false, "Invalid mode: " + modeStr);
  }();
  auto res = co_await env.mgmtdClientGetter()->updateChain(env.userInfo, cid, tid, mode);
  CO_RETURN_ON_ERROR(res);

  statChainInfo(table, *res);
  co_return table;
}

}  // namespace

CoTryTask<void> registerUpdateChainHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
