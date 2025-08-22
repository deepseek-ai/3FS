#include "ListNodes.h"

#include <fmt/chrono.h>
#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/StringUtils.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("list-nodes");
  return parser;
}

void printNode(Dispatcher::OutputTable &table,
               const flat::NodeInfo &node,
               const RHStringHashMap<flat::ConfigVersion> *configVersions) {
  auto nodeType = toStringView(node.type);
  Dispatcher::OutputRow row;
  row.push_back(fmt::format("{}", node.app.nodeId.toUnderType()));
  row.push_back(String(nodeType));
  row.push_back(String(magic_enum::enum_name(node.status)));
  row.push_back(node.app.hostname);
  row.push_back(fmt::format("{}", node.app.pid));
  // TODO: print serviceGroups
  row.push_back(fmt::format("[{}]", fmt::join(node.tags, ",")));
  row.push_back(node.lastHeartbeatTs.toMicroseconds() ? node.lastHeartbeatTs.YmdHMS() : "N/A");

  String configStatus = [&] {
    if (configVersions && configVersions->contains(nodeType)) {
      auto it = configVersions->find(nodeType);
      if (node.configVersion == it->second) {
        switch (node.configStatus) {
          case ConfigStatus::NORMAL:
            return "UPTODATE";
          case ConfigStatus::DIRTY:
            return "DIRTY";
          case ConfigStatus::FAILED:
            return "FAILED";
          default:
            return "";
        }
      }
    }
    return "";
  }();

  if (configStatus.empty()) {
    row.push_back(std::to_string(node.configVersion));
  } else {
    row.push_back(fmt::format("{}({})", node.configVersion.toUnderType(), configStatus));
  }
  row.push_back(fmt::format("{}", node.app.releaseVersion));
  table.push_back(std::move(row));
}

CoTryTask<Dispatcher::OutputTable> handleListNodes(IEnv &ienv,
                                                   const argparse::ArgumentParser &parser,
                                                   const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto mgmtdClient = env.mgmtdClientGetter();
  CO_RETURN_ON_ERROR(co_await mgmtdClient->refreshRoutingInfo(/*force=*/true));
  auto routingInfo = mgmtdClient->getRoutingInfo();
  XLOGF(DBG, "ListNodes routingInfo:{}", serde::toJsonString(*routingInfo->raw()));
  const auto &nodes = routingInfo->raw()->nodes;
  table.push_back(
      {"Id", "Type", "Status", "Hostname", "Pid", "Tags", "LastHeartbeatTime", "ConfigVersion", "ReleaseVersion"});

  auto configVersionsRes = co_await mgmtdClient->getConfigVersions();

  std::vector<flat::NodeInfo> nodeVec;
  for ([[maybe_unused]] auto &[_, node] : nodes) nodeVec.push_back(std::move(node));
  std::sort(nodeVec.begin(), nodeVec.end(), [](const flat::NodeInfo &a, const flat::NodeInfo &b) {
    if (a.type != b.type) return a.type < b.type;
    if (a.status != b.status) return a.status < b.status;
    return a.app.nodeId < b.app.nodeId;
  });
  for (const auto &nodeInfo : nodeVec) printNode(table, nodeInfo, configVersionsRes ? &*configVersionsRes : nullptr);

  co_return table;
}
}  // namespace
CoTryTask<void> registerListNodesHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleListNodes);
}
}  // namespace hf3fs::client::cli
