#include <folly/Conv.h>

#include "AdminEnv.h"
#include "RenderConfig.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/FileUtils.h"
#include "common/utils/StringUtils.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("get-last-config-update-record");
  parser.add_argument("-n", "--node-id").scan<'u', uint32_t>();
  parser.add_argument("-c", "--client-id");
  parser.add_argument("-a", "--addr");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto nodeId = parser.present<uint32_t>("-n");
  auto clientId = parser.present<String>("-c");
  auto addr = parser.present<String>("-a");

  ENSURE_USAGE(nodeId.has_value() + clientId.has_value() + addr.has_value() == 1,
               "must and can only specify one of -n, -c, and -a");

  std::vector<net::Address> addresses;
  if (nodeId) {
    CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(/*force=*/true));
    auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
    auto node = routingInfo->getNode(flat::NodeId(*nodeId));
    if (!node) {
      co_return makeError(MgmtdCode::kNodeNotFound);
    }

    addresses = node->extractAddresses("Core");

    table.push_back({"NodeId", std::to_string(*nodeId)});
  } else if (clientId) {
    auto clientSession = co_await env.mgmtdClientGetter()->getClientSession(*clientId);
    CO_RETURN_ON_ERROR(clientSession);
    if (clientSession->session) {
      addresses = flat::extractAddresses(clientSession->session->serviceGroups, "Core");
    }

    table.push_back({"ClientId", *clientId});
  } else if (addr) {
    auto res = net::Address::from(*addr);
    CO_RETURN_ON_ERROR(res);
    addresses.push_back(*res);

    table.push_back({"Address", *addr});
  }

  auto req = core::GetLastConfigUpdateRecordReq::create();
  auto res = co_await env.coreClientGetter()->getLastConfigUpdateRecord(addresses, req);
  CO_RETURN_ON_ERROR(res);

  if (res->record.has_value()) {
    const auto &record = res->record.value();
    table.push_back({"UpdateTime", record.updateTime.YmdHMS()});
    table.push_back({"Description", record.description});
    table.push_back({"Result", record.result.describe()});
  } else {
    table.push_back({"UpdateTime", "N/A"});
    table.push_back({"Description", "N/A"});
    table.push_back({"Result", "N/A"});
  }

  co_return table;
}
}  // namespace
CoTryTask<void> registerGetLastConfigUpdateRecordHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
