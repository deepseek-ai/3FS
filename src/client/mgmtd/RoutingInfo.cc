#include "RoutingInfo.h"

#include <folly/Random.h>

#include "fbs/mgmtd/RoutingInfo.h"

namespace hf3fs::client {
RoutingInfo::RoutingInfo(std::shared_ptr<flat::RoutingInfo> info, SteadyTime refreshTime)
    : info_(std::move(info)),
      lastRefreshTime_(refreshTime) {}

std::optional<flat::ChainInfo> RoutingInfo::getChain(flat::ChainId id) const {
  if (auto *c = info_ ? info_->getChain(id) : nullptr) {
    return *c;
  }
  return std::nullopt;
}

std::optional<flat::ChainInfo> RoutingInfo::getChain(flat::ChainRef ref) const {
  if (auto *c = info_ ? info_->getChain(ref) : nullptr) {
    return *c;
  }
  return std::nullopt;
}

std::optional<flat::TargetInfo> RoutingInfo::getTarget(flat::TargetId id) const {
  if (auto *t = info_ ? info_->getTarget(id) : nullptr) {
    return *t;
  }
  return std::nullopt;
}

std::optional<flat::NodeInfo> RoutingInfo::getNode(flat::NodeId id) const {
  if (auto *n = info_ ? info_->getNode(id) : nullptr) {
    return *n;
  }
  return std::nullopt;
}

void logUnavailableChains(std::shared_ptr<RoutingInfo> routingInfo) {
  XLOGF_IF(DFATAL, !routingInfo, "empty routinginfo");
  if (const auto &raw = routingInfo->raw(); raw) {
    if (raw->bootstrapping) {
      XLOGF(WARN, "Skip log unavailable chains since mgmtd is bootstrapping");
    }
    for (const auto &[cid, ci] : raw->chains) {
      auto it = std::find_if(ci.targets.begin(), ci.targets.end(), [](const auto &cti) {
        return cti.publicState == flat::PublicTargetState::SERVING;
      });
      if (it == ci.targets.end()) {
        XLOGF(WARN,
              "Found unavailable chain in new RoutingInfo {}: {}",
              raw->routingInfoVersion,
              serde::toJsonString(ci));
      }
    }
  }
}
}  // namespace hf3fs::client
