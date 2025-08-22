#include "ListTargets.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/StringUtils.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("list-targets");
  parser.add_argument("-c", "--chain-id").scan<'u', uint32_t>();
  parser.add_argument("--orphan").default_value(false).implicit_value(true);
  return parser;
}

Result<Void> printTargetsOfChain(Dispatcher::OutputTable &table,
                                 const flat::ChainInfo &ci,
                                 const flat::RoutingInfo::TargetMap &targets) {
  auto n = ci.targets.size();
  for (size_t i = 0; i < n; ++i) {
    const auto &cti = ci.targets[i];
    if (!targets.contains(cti.targetId)) {
      return makeError(StatusCode::kUnknown,
                       fmt::format("{} of {} not found in RoutingInfo", cti.targetId, ci.chainId));
    }
    const auto &ti = targets.at(cti.targetId);
    auto role = i == 0 ? "HEAD" : (i == n - 1 ? "TAIL" : "MIDDLE");
    table.push_back({std::to_string(cti.targetId),
                     std::to_string(ci.chainId),
                     role,
                     toString(ti.publicState),
                     toString(ti.localState),
                     ti.nodeId ? std::to_string(*ti.nodeId) : "N/A",
                     ti.diskIndex ? std::to_string(*ti.diskIndex) : "N/A",
                     std::to_string(ti.usedSize)});
  }
  return Void{};
}

Result<Void> printOrphanTargets(Dispatcher::OutputTable &table, const std::vector<flat::TargetInfo> &targets) {
  table.push_back({"TargetId", "LocalState", "NodeId", "DiskIndex", "UsedSize"});
  for (const auto &ti : targets) {
    table.push_back({std::to_string(ti.targetId),
                     toString(ti.localState),
                     ti.nodeId ? std::to_string(*ti.nodeId) : "N/A",
                     ti.diskIndex ? std::to_string(*ti.diskIndex) : "N/A",
                     std::to_string(ti.usedSize)});
  }
  return Void{};
}

CoTryTask<Dispatcher::OutputTable> handleListTargets(IEnv &ienv,
                                                     const argparse::ArgumentParser &parser,
                                                     const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto orphan = parser.get<bool>("--orphan");
  if (orphan) {
    auto rsp = co_await env.mgmtdClientGetter()->listOrphanTargets();
    CO_RETURN_ON_ERROR(rsp);
    printOrphanTargets(table, rsp->targets);
  } else {
    auto chainId = parser.present<uint32_t>("-c");

    CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(/*force=*/true));
    auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
    XLOGF(DBG, "ListTargets routingInfo:{}", serde::toJsonString(*routingInfo->raw()));
    auto head = std::vector<String>{"TargetId",
                                    "ChainId",
                                    "Role",
                                    "PublicState",
                                    "LocalState",
                                    "NodeId",
                                    "DiskIndex",
                                    "UsedSize"};
    table.push_back(std::move(head));
    if (chainId) {
      auto cid = flat::ChainId(*chainId);
      auto *ci = routingInfo->raw()->getChain(cid);
      if (!ci) co_return makeError(StatusCode::kInvalidArg, fmt::format("{} not found", cid));

      CO_RETURN_ON_ERROR(printTargetsOfChain(table, *ci, routingInfo->raw()->targets));
    } else {
      const auto &chains = routingInfo->raw()->chains;
      const auto &targets = routingInfo->raw()->targets;
      for (const auto &[_, ci] : chains) {
        CO_RETURN_ON_ERROR(printTargetsOfChain(table, ci, targets));
      }
    }
  }

  co_return table;
}
}  // namespace
CoTryTask<void> registerListTargetsHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleListTargets);
}
}  // namespace hf3fs::client::cli
