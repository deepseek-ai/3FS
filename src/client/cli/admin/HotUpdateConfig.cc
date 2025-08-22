#include "HotUpdateConfig.h"

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/FileUtils.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Result.h"
#include "common/utils/StringUtils.h"
#include "fbs/core/service/Rpc.h"
#include "fbs/mgmtd/MgmtdTypes.h"

namespace hf3fs::client::cli {
namespace {
const std::set<String> typeChoices(magic_enum::enum_names<flat::NodeType>().begin(),
                                   magic_enum::enum_names<flat::NodeType>().end());

auto getParser() {
  argparse::ArgumentParser parser("hot-update-config");
  parser.add_argument("-n", "--node-id").scan<'u', uint32_t>();
  parser.add_argument("-t", "--node-type")
      .help(fmt::format("choices : {}", fmt::join(typeChoices.begin(), typeChoices.end(), " | ")));
  parser.add_argument("-c", "--client-id");
  parser.add_argument("-a", "--addr");
  parser.add_argument("-s", "--string");
  parser.add_argument("-f", "--file");
  parser.add_argument("--render").default_value(false);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto nodeId = parser.present<uint32_t>("-n");
  auto type = parser.present<String>("-t");
  auto clientId = parser.present<String>("-c");
  auto addr = parser.present<String>("-a");
  auto str = parser.present<String>("-s");
  auto filePath = parser.present<String>("-f");
  auto render = parser.get<bool>("--render");

  ENSURE_USAGE(nodeId.has_value() + clientId.has_value() + addr.has_value() + type.has_value() == 1,
               "must and can only specify one of -n, -t, -c, and -a");
  ENSURE_USAGE(str.has_value() + filePath.has_value() == 1, "must and can only specify one of -s and -f");

  auto updateString = [&]() -> Result<String> {
    if (str.has_value()) return *str;
    return loadFile(*filePath);
  }();
  CO_RETURN_ON_ERROR(updateString);

  try {
    [[maybe_unused]] auto result = toml::parse(*updateString);
  } catch (const toml::parse_error &e) {
    std::stringstream ss;
    ss << e;
    XLOGF(ERR, "Parse config failed: {}", ss.str());
    co_return makeError(StatusCode::kConfigParseError, fmt::format("Invalid toml: {}", e.what()));
  } catch (std::exception &e) {
    co_return makeError(StatusCode::kConfigInvalidValue, fmt::format("Invalid toml: {}", e.what()));
  }

  auto doUpdate = [&](auto addresses) -> CoTryTask<core::HotUpdateConfigRsp> {
    auto req = core::HotUpdateConfigReq::create(*updateString, render);
    co_return co_await env.coreClientGetter()->hotUpdateConfig(addresses, req);
  };

  if (type) {
    auto ntype = magic_enum::enum_cast<flat::NodeType>(*type);
    ENSURE_USAGE(ntype, fmt::format("invalid type: {}", *type));
    CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(/*force=*/true));
    auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
    auto nodes = routingInfo->getNodeBy(flat::selectNodeByType(*ntype) && flat::selectActiveNode());
    for (const auto &node : nodes) {
      auto addrs = node.extractAddresses("Core");
      auto result = co_await doUpdate(addrs);
      table.push_back(
          {"NodeId", std::to_string(node.app.nodeId), result.hasError() ? result.error().describe() : "success"});
    }

    co_return table;
  }

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
  }
  CO_RETURN_ON_ERROR(co_await doUpdate(addresses));
  table.push_back({"Succeed"});

  co_return table;
}
}  // namespace
CoTryTask<void> registerHotUpdateConfigHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
