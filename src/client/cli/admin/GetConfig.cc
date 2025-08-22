#include "GetConfig.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/FileUtils.h"
#include "common/utils/StringUtils.h"

namespace hf3fs::client::cli {
namespace {
const std::map<String, flat::NodeType> typeMappings = {
    {"MGMTD", flat::NodeType::MGMTD},
    {"META", flat::NodeType::META},
    {"STORAGE", flat::NodeType::STORAGE},
    {"CLIENT", flat::NodeType::CLIENT},
    {"CLIENT_AGENT", flat::NodeType::CLIENT},
    {"FUSE", flat::NodeType::FUSE},
};

const std::set<String> &typeChoices() {
  static auto choices = [] {
    std::set<String> s;
    for (const auto &[k, _] : typeMappings) {
      s.insert(k);
    }
    return s;
  }();
  return choices;
}

auto getParser() {
  argparse::ArgumentParser parser("get-config");
  parser.add_argument("-n", "--node-id").scan<'u', uint32_t>();
  parser.add_argument("-c", "--client-id");
  parser.add_argument("-a", "--addr");
  parser.add_argument("-t", "--node-type").help(fmt::format("choices : {}", fmt::join(typeChoices(), " | ")));
  parser.add_argument("-l", "--list-versions")
      .help("list versions of all types")
      .default_value(false)
      .implicit_value(true);
  parser.add_argument("-k", "--config-key");
  parser.add_argument("-o", "--output-file").help("the path where config is saved at");
  return parser;
}

CoTryTask<void> handleConfigOfType(Dispatcher::OutputTable &table,
                                   IMgmtdClientForAdmin &client,
                                   flat::NodeType t,
                                   std::optional<String> outputFile) {
  auto configRes = co_await client.getConfig(t, flat::ConfigVersion(0));
  CO_RETURN_ON_ERROR(configRes);

  const auto &optionalConfig = *configRes;
  table.push_back({"NodeType", toString(t)});
  if (optionalConfig) {
    table.push_back({"ConfigVersion", std::to_string(optionalConfig->configVersion)});
    table.push_back({"ConfigDesc", optionalConfig->desc});
    if (outputFile) {
      CO_RETURN_ON_ERROR(storeToFile(*outputFile, optionalConfig->content));
      table.push_back({"OutputFile", *outputFile});
    }

  } else {
    table.push_back({"ConfigVersion", "null"});
  }

  co_return Void{};
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
  auto type = parser.present<String>("-t");
  auto listVersions = parser.get<bool>("-l");
  auto configKey = parser.present<String>("-k").value_or("");
  auto outputFile = parser.present<String>("-o");

  ENSURE_USAGE(nodeId.has_value() + clientId.has_value() + type.has_value() + addr.has_value() + listVersions == 1,
               "must and can only specify one of -n, -c, -t, -a, and -l");

  if (nodeId) {
    ENSURE_USAGE(outputFile.has_value(), "must specify -o with -n");

    CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(/*force=*/true));
    auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
    auto node = routingInfo->getNode(flat::NodeId(*nodeId));
    if (!node) {
      co_return makeError(MgmtdCode::kNodeNotFound);
    }

    auto addresses = node->extractAddresses("Core");
    auto res = co_await env.coreClientGetter()->getConfig(addresses, core::GetConfigReq::create(configKey));
    CO_RETURN_ON_ERROR(res);

    table.push_back({"NodeId", std::to_string(*nodeId)});

    CO_RETURN_ON_ERROR(storeToFile(*outputFile, res->config));
    table.push_back({"OutputFile", *outputFile});
  } else if (type) {
    ENSURE_USAGE(typeMappings.contains(*type), fmt::format("invalid type: {}", *type));
    ENSURE_USAGE(outputFile.has_value(), "must specify -o with -t");

    CO_RETURN_ON_ERROR(
        co_await handleConfigOfType(table, *env.mgmtdClientGetter(), typeMappings.at(*type), outputFile));
  } else if (clientId) {
    ENSURE_USAGE(outputFile.has_value(), "must specify -o with -c");

    auto clientSession = co_await env.mgmtdClientGetter()->getClientSession(*clientId);
    CO_RETURN_ON_ERROR(clientSession);

    if (clientSession->session) {
      auto addresses = flat::extractAddresses(clientSession->session->serviceGroups, "Core");
      auto res = co_await env.coreClientGetter()->getConfig(addresses, core::GetConfigReq::create(configKey));
      CO_RETURN_ON_ERROR(res);

      table.push_back({"ClientId", *clientId});

      CO_RETURN_ON_ERROR(storeToFile(*outputFile, res->config));
      table.push_back({"OutputFile", *outputFile});
    } else {
      table.push_back({"Session not found"});
      table.push_back({"ClientId", *clientId});
      table.push_back({"MgmtdBootstrapping", clientSession->bootstrapping ? "Yes" : "No"});
    }
  } else if (addr) {
    ENSURE_USAGE(outputFile.has_value(), "must specify -o with -a");

    auto addrRes = net::Address::from(*addr);
    CO_RETURN_ON_ERROR(addrRes);

    auto res = co_await env.coreClientGetter()->getConfig(std::vector{*addrRes}, core::GetConfigReq::create(configKey));
    CO_RETURN_ON_ERROR(res);

    table.push_back({"Address", *addr});

    CO_RETURN_ON_ERROR(storeToFile(*outputFile, res->config));
    table.push_back({"OutputFile", *outputFile});
  } else {
    ENSURE_USAGE(listVersions);

    table.push_back({"Type", "ConfigVersion"});

    RHStringHashMap<flat::ConfigVersion> versions;
    auto res = co_await env.mgmtdClientGetter()->getConfigVersions();
    if (res) {
      versions = std::move(*res);
    } else {
      auto values = magic_enum::enum_values<flat::NodeType>();
      for (auto t : values) {
        auto configRes = co_await env.mgmtdClientGetter()->getConfig(t, flat::ConfigVersion(0));
        CO_RETURN_ON_ERROR(configRes);
        const auto &optionalConfig = *configRes;
        if (optionalConfig) {
          versions[toString(t)] = optionalConfig->configVersion;
        } else {
          versions[toString(t)] = flat::ConfigVersion(0);
        }
      }
    }
    for (const auto &[k, v] : versions) {
      table.push_back({k, v == 0 ? "N/A" : std::to_string(v)});
    }
  }

  co_return table;
}
}  // namespace
CoTryTask<void> registerGetConfigHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
