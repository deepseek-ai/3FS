#include "DumpChains.h"

#include <folly/Conv.h>
#include <fstream>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("dump-chains");
  parser.add_argument("csv-file-path-prefix");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleDumpChains(IEnv &ienv,
                                                    const argparse::ArgumentParser &parser,
                                                    const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto csvFilePathPrefix = parser.get<std::string>("csv-file-path-prefix");

  CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(/*force=*/true));
  auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
  XLOGF(DBG, "DumpChains routingInfo:{}", serde::toJsonString(*routingInfo->raw()));

  robin_hood::unordered_map<size_t, std::vector<flat::ChainId>> chainsByReplicaCount;

  for (const auto &[cid, ci] : routingInfo->raw()->chains) {
    chainsByReplicaCount[ci.targets.size()].push_back(cid);
  }

  for (const auto &[rc, chains] : chainsByReplicaCount) {
    auto path = fmt::format("{}.{}", csvFilePathPrefix, rc);
    std::ofstream of(path);
    of.exceptions(std::ofstream::failbit | std::ofstream::badbit);
    of << "ChainId";
    for (size_t i = 0; i < rc; ++i) of << ",TargetId";
    of << "\n";
    for (auto cid : chains) {
      const auto &ci = routingInfo->raw()->chains[cid];
      of << cid.toUnderType();
      for (const auto &ti : ci.targets) of << "," << ti.targetId.toUnderType();
      of << "\n";
    }
    table.push_back({fmt::format("Dump {} chains of {} replicas to {} succeeded", chains.size(), rc, path)});
  }

  co_return table;
}
}  // namespace
CoTryTask<void> registerDumpChainsHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleDumpChains);
}
}  // namespace hf3fs::client::cli
