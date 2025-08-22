#include "Utils.h"

#include <fstream>
#include <sstream>

namespace hf3fs::client::cli {
void statNode(Dispatcher::OutputTable &table, const flat::NodeInfo &node) {
  table.push_back({"NodeId", fmt::format("{}", node.app.nodeId.toUnderType())});
  table.push_back({"Type", String(magic_enum::enum_name(node.type))});
  table.push_back({"Status", String(magic_enum::enum_name(node.status))});
  table.push_back({"Hostname", node.app.hostname});
  table.push_back({"Pid", fmt::format("{}", node.app.pid)});
  // TODO: print releaseVersion
  table.push_back({"LastHeartbeatTime", fmt::format("{}", node.lastHeartbeatTs.toMicroseconds())});
  std::vector<String> tags;
  for (const auto &tp : node.tags) {
    if (!tp.value.empty()) {
      tags.push_back(fmt::format("{}={}", tp.key, tp.value));
    } else {
      tags.push_back(tp.key);
    }
  }
  table.push_back({"Tags", fmt::format("[{}]", fmt::join(tags, ","))});
}

void statChainInfo(Dispatcher::OutputTable &table, const flat::ChainInfo &chain) {
  table.push_back({"ChainId", std::to_string(chain.chainId)});
  table.push_back({"ChainVersion", std::to_string(chain.chainVersion)});
  for (const auto &ti : chain.targets) {
    auto s = fmt::format("{}({})", ti.targetId.toUnderType(), magic_enum::enum_name(ti.publicState));
    table.push_back({"Target", s});
  }
  auto preferredOrder =
      transformTo<std::vector>(std::span{chain.preferredTargetOrder.begin(), chain.preferredTargetOrder.size()},
                               [](auto tid) { return tid.toUnderType(); });
  table.push_back({"PreferredOrder", fmt::format("[{}]", fmt::join(preferredOrder, ","))});
}
}  // namespace hf3fs::client::cli
