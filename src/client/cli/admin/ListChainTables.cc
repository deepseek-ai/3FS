#include "ListChainTables.h"

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("list-chain-tables");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleListChainTables(IEnv &ienv,
                                                         const argparse::ArgumentParser &parser,
                                                         const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(/*force=*/true));
  auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
  XLOGF(DBG, "ListChainTables routingInfo:{}", serde::toJsonString(*routingInfo->raw()));
  table.push_back({"ChainTableId", "ChainTableVersion", "ChainCount", "ReplicaCount", "Desc"});
  for ([[maybe_unused]] const auto &[tableId, chainTables] : routingInfo->raw()->chainTables) {
    for ([[maybe_unused]] const auto &[tv, chainTable] : chainTables) {
      auto chainCount = chainTable.chains.size();
      if (chainCount == 0) {
        co_return makeError(StatusCode::kUnknown, fmt::format("Impossible: {} has no chains", tableId));
      }
      std::set<size_t> replicaCount;
      for (auto i = 0u; i < chainCount; ++i) {
        auto *ci = routingInfo->raw()->getChain(chainTable.chains[i]);
        if (!ci) {
          co_return makeError(MgmtdClientCode::kRoutingInfoNotReady);
        }
        replicaCount.insert(ci->targets.size());
      }
      table.push_back({std::to_string(tableId.toUnderType()),
                       std::to_string(tv.toUnderType()),
                       std::to_string(chainCount),
                       fmt::format("{}", fmt::join(replicaCount, "/")),
                       chainTable.desc});
    }
  }

  co_return table;
}
}  // namespace
CoTryTask<void> registerListChainTablesHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleListChainTables);
}
}  // namespace hf3fs::client::cli
