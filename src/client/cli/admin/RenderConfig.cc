#include "RenderConfig.h"

#include <folly/Conv.h>
#include <fstream>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/FileUtils.h"
#include "common/utils/RenderConfig.h"
#include "common/utils/StringUtils.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("render-config");
  parser.add_argument("-n", "--node-id").scan<'u', uint32_t>();
  parser.add_argument("-c", "--client-id");
  parser.add_argument("-a", "--addr");
  parser.add_argument("-u", "--test-update").default_value(false).implicit_value(true);
  parser.add_argument("--output-updated-config").default_value(false).implicit_value(true);
  parser.add_argument("--hot").default_value(false).implicit_value(true);
  parser.add_argument("-f", "--template-file").required();
  parser.add_argument("-o", "--output-file").required();
  parser.add_argument("--mock").default_value(false).implicit_value(true);
  parser.add_argument("--mock-node-id").scan<'u', uint32_t>();
  parser.add_argument("--mock-hostname");
  parser.add_argument("--mock-pid").scan<'u', uint32_t>();
  parser.add_argument("--mock-cluster-id");
  parser.add_argument("--mock-tags").help("format: k0=v0 or k1").nargs(argparse::nargs_pattern::any);
  parser.add_argument("--mock-release-version")
      .help(fmt::format("default is release version of current binary ({})", flat::ReleaseVersion::fromVersionInfo()));
  parser.add_argument("--mock-envs").help("format: k0=v0").nargs(argparse::nargs_pattern::any);
  parser.add_argument("--mock-raw-output").default_value(false).implicit_value(true);
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
  auto testUpdate = parser.get<bool>("-u");
  auto outputUpdatedConfig = parser.get<bool>("--output-updated-config");
  auto hotUpdate = parser.get<bool>("--hot");
  auto templateFile = parser.get<String>("-f");
  auto outputFile = parser.get<String>("-o");

  auto mock = parser.get<bool>("--mock");
  auto mockNodeId = parser.present<uint32_t>("--mock-node-id");
  auto mockHostname = parser.present<String>("--mock-hostname");
  auto mockPid = parser.present<uint32_t>("--mock-pid");
  auto mockClusterId = parser.present<String>("--mock-cluster-id");
  auto mockTags = parser.get<std::vector<String>>("--mock-tags");
  auto mockReleaseVersion = parser.present<String>("--mock-release-version");
  auto mockEnvs = parser.get<std::vector<String>>("--mock-envs");
  auto mockRawOutput = parser.get<bool>("--mock-raw-output");

  ENSURE_USAGE(nodeId.has_value() + clientId.has_value() + addr.has_value() + mock == 1,
               "must and can only specify one of -n, -c, -a, and --mock");

  ENSURE_USAGE(!outputUpdatedConfig || testUpdate, "must set --output-updated-config with -u");

  auto loadFileRes = loadFile(templateFile);
  CO_RETURN_ON_ERROR(loadFileRes);

  if (mock) {
    flat::AppInfo appInfo;
    appInfo.nodeId = flat::NodeId(mockNodeId.value_or(appInfo.nodeId));
    appInfo.hostname = mockHostname.value_or(appInfo.hostname);
    appInfo.pid = mockPid.value_or(appInfo.pid);
    appInfo.clusterId = mockClusterId.value_or(appInfo.clusterId);

    for (const auto &tag : mockTags) {
      auto pos = tag.find('=');
      if (pos == String::npos) {
        appInfo.tags.emplace_back(tag);
      } else if (pos == 0) {
        co_return MAKE_ERROR_F(StatusCode::kInvalidArg, "Invalid tag: {}", tag);
      } else {
        appInfo.tags.emplace_back(tag.substr(0, pos), tag.substr(pos + 1));
      }
    }

    appInfo.releaseVersion = flat::ReleaseVersion::fromVersionInfo();
    if (mockReleaseVersion) {
      auto parseRes = parseReleaseVersion(*mockReleaseVersion, appInfo.releaseVersion);
      CO_RETURN_ON_ERROR(parseRes);
      appInfo.releaseVersion = *parseRes;
    }

    std::map<String, String> envs;
    for (const auto &env : mockEnvs) {
      auto pos = env.find('=');
      if (pos == String::npos || pos == 0) {
        co_return MAKE_ERROR_F(StatusCode::kInvalidArg, "Invalid env: {}", env);
      } else {
        envs.emplace(env.substr(0, pos), env.substr(pos + 1));
      }
    }

    table.push_back({"app.nodeId", std::to_string(appInfo.nodeId)});
    table.push_back({"app.hostname", appInfo.hostname});
    table.push_back({"app.pid", std::to_string(appInfo.pid)});
    table.push_back({"app.tags", serde::toJsonString(appInfo.tags)});
    table.push_back({"app.clusterId", appInfo.clusterId});
    table.push_back({"app.releaseVersion", appInfo.releaseVersion.toString()});
    table.push_back({"delta-envs", serde::toJsonString(envs)});
    table.push_back({"rawOutput", mockRawOutput ? "true" : "false"});

    auto renderRes = renderConfig(*loadFileRes, &appInfo, &envs);
    CO_RETURN_ON_ERROR(renderRes);

    if (!mockRawOutput) {
      auto t = toml::parse(*renderRes);
      std::stringstream ss;
      ss << toml::toml_formatter(t, toml::toml_formatter::default_flags & ~toml::format_flags::indentation);
      *renderRes = ss.str();
    }

    auto storeRes = storeToFile(outputFile, *renderRes);
    CO_RETURN_ON_ERROR(storeRes);

    table.push_back({"OutputFile", outputFile});
  } else {
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
    auto req = core::RenderConfigReq::create(*loadFileRes, testUpdate, hotUpdate);
    auto res = co_await env.coreClientGetter()->renderConfig(addresses, req);
    CO_RETURN_ON_ERROR(res);

    if (!outputUpdatedConfig) {
      std::ofstream of(outputFile);
      of.exceptions(std::ofstream::failbit | std::ofstream::badbit);
      of << res->configAfterRender;
    } else if (res->updateStatus.isOK()) {
      std::ofstream of(outputFile);
      of.exceptions(std::ofstream::failbit | std::ofstream::badbit);
      of << res->configAfterUpdate;
    }

    table.push_back({"OutputFile", outputFile});
    if (testUpdate) {
      table.push_back({hotUpdate ? "HotUpdate" : "ColdUpdate", res->updateStatus.describe()});
    }
  }

  co_return table;
}
}  // namespace
CoTryTask<void> registerRenderConfigHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
