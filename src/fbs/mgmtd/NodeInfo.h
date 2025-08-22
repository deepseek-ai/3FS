#pragma once

#include <boost/algorithm/string.hpp>
#include <span>

#include "MgmtdTypes.h"
#include "common/app/AppInfo.h"
#include "common/app/ConfigStatus.h"
#include "common/serde/SerdeHelper.h"
#include "common/utils/Selector.h"
#include "common/utils/UtcTimeSerde.h"

namespace hf3fs::flat {

struct NodeInfo : public serde::SerdeHelper<NodeInfo> {
  std::vector<net::Address> extractAddresses(const String &serviceName,
                                             std::optional<net::Address::Type> addressType = std::nullopt) const;

  std::map<String, std::vector<net::Address>> getAllServices() const;

  bool operator==(const NodeInfo &other) const { return serde::equals(*this, other); }

  SERDE_STRUCT_FIELD(app, FbsAppInfo{});
  SERDE_STRUCT_FIELD(type, NodeType(NodeType::MGMTD));
  SERDE_STRUCT_FIELD(status, NodeStatus(NodeStatus::HEARTBEAT_CONNECTING));
  SERDE_STRUCT_FIELD(lastHeartbeatTs, UtcTime{});
  SERDE_STRUCT_FIELD(tags, std::vector<TagPair>{});
  SERDE_STRUCT_FIELD(configVersion, ConfigVersion(0));
  SERDE_STRUCT_FIELD(configStatus, ConfigStatus::NORMAL);
};

inline auto selectNodeByType(flat::NodeType type) {
  auto p = [type](const flat::NodeInfo &node) { return node.type == type; };
  return makeSelector<flat::NodeInfo>(std::move(p));
}

inline auto selectActiveNode() {
  auto p = [](const flat::NodeInfo &node) {
    return node.status == flat::NodeStatus::HEARTBEAT_CONNECTED || node.status == flat::NodeStatus::PRIMARY_MGMTD;
  };
  return makeSelector<flat::NodeInfo>(std::move(p));
}

inline auto selectNodeById(flat::NodeId id) {
  auto p = [id](const flat::NodeInfo &node) { return node.app.nodeId == id; };
  return makeSelector<flat::NodeInfo>(std::move(p));
}

inline auto selectNodeByStatus(flat::NodeStatus status) {
  auto p = [status](const flat::NodeInfo &node) { return node.status == status; };
  return makeSelector<flat::NodeInfo>(std::move(p));
}

inline auto selectNodeByTrafficZone(std::string_view zone) {
  std::vector<std::string_view> zones;
  if (!zone.empty()) {
    std::vector<std::string_view> tmp;
    boost::split(tmp, zone, boost::is_any_of(", "));
    std::copy_if(tmp.begin(), tmp.end(), std::back_inserter(zones), [](std::string_view s) { return !s.empty(); });
  }
  auto p = [zones = std::move(zones)](const flat::NodeInfo &node) {
    if (zones.empty()) return true;  // select any node if zone is empty
    for (const auto &tp : node.tags) {
      if (tp.key == kTrafficZoneTagKey && std::find(zones.begin(), zones.end(), tp.value) != zones.end()) return true;
    }
    return false;
  };
  return makeSelector<flat::NodeInfo>(std::move(p));
}

}  // namespace hf3fs::flat
