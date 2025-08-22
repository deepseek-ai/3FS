#pragma once

#include "common/app/AppInfo.h"
#include "common/utils/Selector.h"
#include "fbs/mgmtd/MgmtdTypes.h"

namespace hf3fs::client {
struct ServiceInfo {
  String name;
  uint16_t id{0};
  flat::NodeId nodeId{0};
  flat::NodeStatus nodeStatus{flat::NodeStatus::HEARTBEAT_CONNECTING};
  std::vector<net::Address> endpoints;

  std::vector<net::Address> filterAddress(net::Address::Type type) const {
    std::vector<net::Address> addresses;
    std::copy_if(endpoints.begin(), endpoints.end(), std::back_inserter(addresses), [&](const auto &addr) {
      return addr.type == type;
    });
    return addresses;
  }

  ServiceInfo() = default;
};
}  // namespace hf3fs::client
