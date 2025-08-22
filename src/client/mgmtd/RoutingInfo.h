#pragma once

#include "ServiceInfo.h"
#include "common/utils/Selector.h"
#include "common/utils/UtcTime.h"
#include "fbs/mgmtd/RoutingInfo.h"

namespace hf3fs::client {

class RoutingInfo {
 public:
  RoutingInfo() = default;
  RoutingInfo(std::shared_ptr<flat::RoutingInfo> info, SteadyTime refreshTime);

  const auto &raw() const { return info_; }

  SteadyTime lastRefreshTime() const { return lastRefreshTime_; }

  std::optional<flat::ChainInfo> getChain(flat::ChainId id) const;
  std::optional<flat::ChainInfo> getChain(flat::ChainRef ref) const;
  std::optional<flat::TargetInfo> getTarget(flat::TargetId id) const;
  std::optional<flat::NodeInfo> getNode(flat::NodeId id) const;

  template <UnaryPredicate<flat::NodeInfo> NodeSelector>
  std::vector<flat::NodeInfo> getNodeBy(const NodeSelector &selector) const {
    std::vector<flat::NodeInfo> res;
    if (info_) {
      for ([[maybe_unused]] const auto &[id, info] : info_->nodes) {
        if (selector(info)) res.push_back(info);
      }
    }
    return res;
  }

  template <UnaryPredicate<flat::NodeInfo> NodeSelector, UnaryPredicate<ServiceInfo> ServiceSelector>
  std::vector<ServiceInfo> getServiceBy(const NodeSelector &nodeSelector,
                                        const ServiceSelector &serviceSelector) const {
    std::vector<ServiceInfo> res;
    if (info_) {
      for ([[maybe_unused]] const auto &[id, node] : info_->nodes) {
        if (nodeSelector(node)) {
          auto smap = node.getAllServices();
          for (auto &[si, addrVec] : smap) {
            ServiceInfo info;
            info.name = si;
            info.nodeId = node.app.nodeId;
            info.nodeStatus = node.status;
            info.endpoints = std::move(addrVec);

            if (serviceSelector(info)) {
              res.push_back(std::move(info));
            }
          }
        }
      }
    }
    return res;
  }

 private:
  std::shared_ptr<flat::RoutingInfo> info_;
  SteadyTime lastRefreshTime_;
};

void logUnavailableChains(std::shared_ptr<RoutingInfo> routingInfo);
}  // namespace hf3fs::client
