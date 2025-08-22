#include "DumpChainTable.h"

#include <folly/Conv.h>
#include <fstream>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("dump-chain-table");
  parser.add_argument("tableId").scan<'u', uint32_t>();
  parser.add_argument("-v", "--version").scan<'u', uint32_t>();
  parser.add_argument("csv-file-path");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleDumpChainTable(IEnv &ienv,
                                                        const argparse::ArgumentParser &parser,
                                                        const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto tableId = flat::ChainTableId(parser.get<uint32_t>("tableId"));
  auto tableVer = flat::ChainTableVersion(parser.present<uint32_t>("-v").value_or(0));
  auto csvFilePath = parser.get<std::string>("csv-file-path");

  std::ofstream of(csvFilePath);
  of.exceptions(std::ofstream::failbit | std::ofstream::badbit);

  CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(/*force=*/true));
  auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
  XLOGF(DBG, "DumpChainTable routingInfo:{}", serde::toJsonString(*routingInfo->raw()));
  auto *t = routingInfo->raw()->getChainTable(tableId, tableVer);
  if (!t) co_return makeError(StatusCode::kInvalidArg, fmt::format("{}@{} not found", tableId, tableVer));
  tableVer = t->chainTableVersion;

  of << "ChainId\n";
  for (auto cid : t->chains) {
    of << cid.toUnderType() << "\n";
  }

  table.push_back({fmt::format("Dump {} of {} to {} succeeded", tableId, tableVer, csvFilePath)});
  co_return table;
}
}  // namespace
CoTryTask<void> registerDumpChainTableHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleDumpChainTable);
}
}  // namespace hf3fs::client::cli
