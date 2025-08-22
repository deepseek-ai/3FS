#include "ListChains.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("list-chains");
  parser.add_argument("-t", "--table-id").scan<'u', uint32_t>();
  parser.add_argument("-v", "--version").scan<'u', uint32_t>();
  return parser;
}

void printChain(Dispatcher::OutputRow &row, const flat::ChainInfo &ci, const flat::RoutingInfo::TargetMap &targets) {
  row.push_back(std::to_string(ci.chainVersion));
  auto statusPos = row.size();
  row.push_back("placeholder");

  auto preferredOrder =
      transformTo<std::vector>(std::span{ci.preferredTargetOrder.begin(), ci.preferredTargetOrder.size()},
                               [](auto tid) { return tid.toUnderType(); });
  row.push_back(fmt::format("[{}]", fmt::join(preferredOrder, ",")));

  size_t serving = 0, syncing = 0;
  for (const auto &t : ci.targets) {
    const auto &ti = targets.at(t.targetId);
    row.push_back(fmt::format("{}({}-{})",
                              t.targetId.toUnderType(),
                              magic_enum::enum_name(ti.publicState),
                              magic_enum::enum_name(ti.localState)));
    using PS = flat::PublicTargetState;
    switch (ti.publicState) {
      case PS::SERVING:
        ++serving;
        break;
      case PS::SYNCING:
        ++syncing;
        break;
      case PS::INVALID:
      case PS::LASTSRV:
      case PS::WAITING:
      case PS::OFFLINE:
        break;
    }
  }
  auto &status = row[statusPos];
  if (syncing) {
    status = "SYNCING";
  } else if (serving == ci.targets.size()) {
    status = "SERVING";
  } else if (serving == 0) {
    status = "UNAVAILABLE";
  } else {
    status = fmt::format("SERVING({}/{})", serving, ci.targets.size());
  }
}

void printChainTable(Dispatcher::OutputTable &table,
                     const flat::RoutingInfo::ChainMap &chains,
                     const flat::ChainTable &ct,
                     const flat::RoutingInfo::TargetMap &targets) {
  for (const auto &c : ct.chains) {
    const auto &ci = chains.at(c);
    std::vector<String> v = {
        fmt::format("{}", ci.chainId.toUnderType()),
        std::to_string(ct.chainTableId.toUnderType()),
        std::to_string(ct.chainTableVersion.toUnderType()),
    };
    printChain(v, ci, targets);
    table.push_back(std::move(v));
  }
}

CoTryTask<Dispatcher::OutputTable> handleListChains(IEnv &ienv,
                                                    const argparse::ArgumentParser &parser,
                                                    const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto tableId = parser.present<uint32_t>("-t");
  auto tableVer = parser.present<uint32_t>("-v");

  CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(/*force=*/true));
  auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
  XLOGF(DBG, "ListChains routingInfo:{}", serde::toJsonString(*routingInfo->raw()));
  const auto &chainTables = routingInfo->raw()->chainTables;
  if (tableId) {
    auto tbid = flat::ChainTableId(*tableId);
    auto tbv = flat::ChainTableVersion(tableVer.value_or(0));
    auto *ct = routingInfo->raw()->getChainTable(tbid, tbv);
    if (!ct) co_return makeError(StatusCode::kInvalidArg, fmt::format("{} not found", tbid));
    if (ct->chains.empty()) co_return makeError(StatusCode::kUnknown, fmt::format("{}@{} has no chains", tbid, tbv));
    size_t replicaCount = 0;
    for (auto cid : ct->chains) {
      auto *ci = routingInfo->raw()->getChain(cid);
      if (!ci) co_return makeError(StatusCode::kUnknown, fmt::format("{} not found", cid));
      replicaCount = std::max(replicaCount, ci->targets.size());
    }

    auto head =
        std::vector<String>{"ChainId", "ChainTable", "ChainTableVersion", "ChainVersion", "Status", "PreferredOrder"};
    for (size_t i = 0; i < replicaCount; ++i) {
      head.push_back("Target");
    }
    table.push_back(std::move(head));

    printChainTable(table, routingInfo->raw()->chains, *ct, routingInfo->raw()->targets);
  } else {
    size_t maxTargets = 0;
    for ([[maybe_unused]] const auto &[_, c] : routingInfo->raw()->chains)
      maxTargets = std::max(maxTargets, c.targets.size());

    auto head = std::vector<String>{"ChainId", "ReferencedBy", "ChainVersion", "Status", "PreferredOrder"};
    for (size_t i = 0; i < maxTargets; ++i) head.push_back("Target");
    table.push_back(std::move(head));

    robin_hood::unordered_map<flat::ChainId, std::set<flat::ChainTableId::UnderlyingType>> refs;

    for (const auto &[ctid, vm] : chainTables) {
      for ([[maybe_unused]] const auto &[_, ct] : vm) {
        for (const auto &cid : ct.chains) {
          refs[cid].insert(ctid.toUnderType());
        }
      }
    }

    std::vector<flat::ChainInfo> chains;
    for (const auto &[_, ci] : routingInfo->raw()->chains) {
      chains.push_back(ci);
    }
    std::sort(chains.begin(), chains.end(), [](const auto &a, const auto &b) { return a.chainId < b.chainId; });

    for (const auto &ci : chains) {
      auto cid = ci.chainId;
      Dispatcher::OutputRow row;
      row.push_back(std::to_string(cid));
      const auto &ts = refs[cid];
      if (ts.empty()) {
        row.push_back("N/A");
      } else if (ts.size() == 1) {
        row.push_back(std::to_string(*ts.begin()));
      } else if (ts.size() == 2) {
        row.push_back(fmt::format("{}", fmt::join(ts, ",")));
      } else {
        auto v = std::array{*ts.begin(), *++ts.begin()};
        row.push_back(fmt::format("{},...", fmt::join(v, ",")));
      }
      printChain(row, ci, routingInfo->raw()->targets);
      table.push_back(std::move(row));
    }
  }

  co_return table;
}
}  // namespace
CoTryTask<void> registerListChainsHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleListChains);
}
}  // namespace hf3fs::client::cli
