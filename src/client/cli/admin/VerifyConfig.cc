#include "VerifyConfig.h"

#include <folly/Conv.h>
#include <folly/experimental/coro/Collect.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/FileUtils.h"
#include "common/utils/StringUtils.h"

namespace hf3fs::client::cli {
namespace {
// TODO: consider how to verify agent configs
const std::set<String> typeChoices = {"MGMTD", "META", "STORAGE"};

auto getParser() {
  argparse::ArgumentParser parser("verify-config");
  parser.add_argument("-t", "--node-type").help(fmt::format("choices : {}", fmt::join(typeChoices, " | "))).required();
  parser.add_argument("-f", "--template-file").required();
  parser.add_argument("--verbose").default_value(false).implicit_value(true);
  return parser;
}

String toPrettyToml(const String &s) {
  auto t = toml::parse(s);
  std::stringstream ss;
  ss << toml::toml_formatter(t, toml::toml_formatter::default_flags & ~toml::format_flags::indentation);
  return ss.str();
}

struct VerifyConfigResult {
  flat::NodeId nodeId{0};
  Result<String> currentConfig = makeError(StatusCode::kUnknown);
  Result<core::RenderConfigRsp> coldConfigRes = makeError(StatusCode::kUnknown);
  Result<core::RenderConfigRsp> hotConfigRes = makeError(StatusCode::kUnknown);
};

CoTask<VerifyConfigResult> verifyConfigOn(AdminEnv &env, const flat::NodeInfo &node, const String &config) {
  VerifyConfigResult result = {node.app.nodeId};
  auto addresses = node.extractAddresses("Core");
  auto coreClient = env.coreClientGetter();
  result.currentConfig = (co_await coreClient->getConfig(addresses, core::GetConfigReq::create())).then([](auto &rsp) {
    return rsp.config;
  });
  result.coldConfigRes = co_await coreClient->renderConfig(
      addresses,
      core::RenderConfigReq::create(config, /*testUpdate=*/true, /*hotUpdate=*/false));
  result.hotConfigRes =
      co_await coreClient->renderConfig(addresses,
                                        core::RenderConfigReq::create(config, /*testUpdate=*/true, /*hotUpdate=*/true));

  co_return result;
}

String generateMessage(const String &currentConfig, const Result<core::RenderConfigRsp> &renderRes) {
  if (!renderRes) {
    return fmt::format("RenderConfig failed: {}", renderRes.error());
  } else if (!renderRes->updateStatus.isOK()) {
    return fmt::format("UpdateConfig failed: {}", renderRes->updateStatus);
  } else {
    auto a = toPrettyToml(renderRes->configAfterUpdate);
    auto b = toPrettyToml(currentConfig);
    if (a != b) {
      return "Changed";
    } else {
      return "Unchanged";
    }
  }
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto type = parser.get<String>("-t");
  auto templateFile = parser.get<String>("-f");
  auto verbose = parser.get<bool>("--verbose");

  auto loadFileRes = loadFile(templateFile);
  CO_RETURN_ON_ERROR(loadFileRes);

  ENSURE_USAGE(typeChoices.contains(type), fmt::format("Unknown type: {}", type));
  auto t = magic_enum::enum_cast<flat::NodeType>(type);
  ENSURE_USAGE(t.has_value(), fmt::format("Invalid type: {}", type));

  CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(/*force=*/true));
  auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
  auto nodes = routingInfo->getNodeBy(flat::selectNodeByType(*t) && flat::selectActiveNode());

  auto client = env.clientGetter();

  std::vector<folly::SemiFuture<VerifyConfigResult>> verifyTasks;
  for (const auto &node : nodes) {
    verifyTasks.push_back(
        verifyConfigOn(env, node, *loadFileRes).scheduleOn(&client->tpg().procThreadPool().randomPick()).start());
  }

  auto results = co_await folly::coro::collectAllRange(std::move(verifyTasks));

  std::map<String, int> count;
  for (const auto &vr : results) {
    if (!vr.currentConfig) {
      ++count[fmt::format("get_config_failed({})", StatusCode::toString(vr.currentConfig.error().code()))];
    } else {
      if (!vr.coldConfigRes) {
        ++count[fmt::format("verify_cold_render_failed({})", StatusCode::toString(vr.coldConfigRes.error().code()))];
      } else if (!vr.coldConfigRes->updateStatus.isOK()) {
        ++count[fmt::format("verify_cold_update_failed({})",
                            StatusCode::toString(vr.coldConfigRes->updateStatus.code()))];
      } else {
        auto a = toPrettyToml(vr.coldConfigRes->configAfterUpdate);
        auto b = toPrettyToml(*vr.currentConfig);
        ++count[a == b ? "verify_cold_unchanged" : "verify_cold_changed"];
      }
      if (!vr.hotConfigRes) {
        ++count[fmt::format("verify_hot_render_failed({})", StatusCode::toString(vr.hotConfigRes.error().code()))];
      } else if (!vr.hotConfigRes->updateStatus.isOK()) {
        ++count[fmt::format("verify_hot_update_failed({})",
                            StatusCode::toString(vr.hotConfigRes->updateStatus.code()))];
      } else {
        auto a = toPrettyToml(vr.hotConfigRes->configAfterUpdate);
        auto b = toPrettyToml(*vr.currentConfig);
        ++count[a == b ? "verify_hot_unchanged" : "verify_hot_changed"];
      }
    }
  }

  auto message = fmt::format("total={}", results.size());
  for (const auto &[k, v] : count) {
    fmt::format_to(std::back_inserter(message), " {}={}", k, v);
  }

  table.push_back({message});
  if (verbose) {
    table.push_back({"Details:"});
    for (const auto &vr : results) {
      SCOPE_EXIT { table.push_back({}); };
      table.push_back({"NodeId", std::to_string(vr.nodeId)});
      if (!vr.currentConfig) {
        auto s = fmt::format("GetConfig failed: {}", vr.currentConfig.error());
        table.push_back({"Cold Start", s});
        table.push_back({"Hot Update", s});
        continue;
      }
      table.push_back({"Cold Start", generateMessage(*vr.currentConfig, vr.coldConfigRes)});
      table.push_back({"Hot Update", generateMessage(*vr.currentConfig, vr.hotConfigRes)});
    }

    if (table.back().empty()) {
      table.pop_back();
    }
  }

  co_return table;
}
}  // namespace
CoTryTask<void> registerVerifyConfigHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
