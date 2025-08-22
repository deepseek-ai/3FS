#include "ServerSelectionStrategy.h"

#include <algorithm>
#include <fmt/format.h>
#include <folly/Random.h>
#include <folly/Range.h>
#include <folly/SharedMutex.h>
#include <folly/Synchronized.h>
#include <folly/hash/Hash.h>
#include <folly/logging/xlog.h>
#include <folly/ssl/OpenSSLHash.h>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/mgmtd/RoutingInfo.h"
#include "common/app/NodeId.h"
#include "common/monitor/Recorder.h"
#include "common/utils/Address.h"
#include "common/utils/Result.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Utils.h"

namespace hf3fs::meta::client {

namespace {
monitor::ValueRecorder preferMetaServer("meta_client.prefer_server", std::nullopt, false);
monitor::CountRecorder noMetaAfterSkip("meta_client.no_meta_after_skip");
}  // namespace

bool ServerList::update(std::shared_ptr<RoutingInfo> newRouting, net::Address::Type addrType) {
  if (newRouting == routing) {
    return false;
  }
  XLOGF(DBG, "ServerList get new routing info {}", newRouting->raw()->routingInfoVersion);
  routing = newRouting;
  auto nodes = routing->getNodeBy(flat::selectNodeByType(flat::NodeType::META) && flat::selectActiveNode());
  if (nodes.empty() && !nodeList.empty()) {
    XLOGF(WARN,
          "meta::ServerList ignore routing info version {}, because no active meta server",
          newRouting->raw()->routingInfoVersion);
    return false;
  }

  auto oldNodes = std::exchange(nodeList, {});
  auto oldAddress = std::exchange(nodeAddress, {});
  for (const auto &node : nodes) {
    auto addrs = node.extractAddresses("MetaSerde", addrType);
    if (addrs.empty()) {
      XLOGF(WARN,
            "meta::ServerList get node {} {}@{} without address of type {}",
            node.app.nodeId,
            node.app.hostname,
            node.app.pid,
            magic_enum::enum_name(addrType));
      continue;
    }
    nodeList.push_back(node.app.nodeId);
    nodeAddress[node.app.nodeId] = addrs[0];
  }
  if (oldNodes != nodeList || oldAddress != nodeAddress) {
    XLOGF(INFO,
          "meta::ServerList update routing info {}, find {} meta servers, {}",
          routing->raw()->routingInfoVersion,
          nodeList.size(),
          fmt::join(nodeList, ","));
    return true;
  } else {
    XLOGF(DBG, "meta::ServerList not changed after apply routing info {}", routing->raw()->routingInfoVersion);
  }

  return false;
}

std::optional<ServerSelectionStrategy::NodeInfo> ServerList::get(flat::NodeId nodeId) const {
  if (!nodeAddress.contains(nodeId)) {
    return std::nullopt;
  }

  auto addr = nodeAddress.at(nodeId);
  auto hostname = routing->getNode(nodeId)->app.hostname;
  return ServerSelectionStrategy::NodeInfo{nodeId, addr, hostname};
}

std::unique_ptr<ServerSelectionStrategy> ServerSelectionStrategy::create(ServerSelectionMode mode,
                                                                         std::shared_ptr<ICommonMgmtdClient> mgmtd,
                                                                         net::Address::Type addrType) {
  switch (mode) {
    case ServerSelectionMode::RoundRobin:
      return std::make_unique<RoundRobinServerSelection>(mgmtd, addrType);
    case ServerSelectionMode::UniformRandom:
      return std::make_unique<UniformRandomServerSelection>(mgmtd, addrType);
    case ServerSelectionMode::RandomFollow:
      return std::make_unique<RandomFollowServerSelection>(mgmtd, addrType);
    default:
      throw std::invalid_argument("invalid server selection mode");
  }
}

static std::vector<flat::NodeId> skip(const std::vector<flat::NodeId> allNodes,
                                      const std::set<flat::NodeId> &skipNodes) {
  std::vector<flat::NodeId> nodes;
  nodes.reserve(allNodes.size());
  for (auto node : allNodes) {
    if (!skipNodes.contains(node)) {
      nodes.push_back(node);
    }
  }
  return nodes;
}

BaseSelectionStrategy::BaseSelectionStrategy(std::shared_ptr<ICommonMgmtdClient> mgmtd, net::Address::Type addrType)
    : name_(fmt::format("meta-server-selection-{}", Uuid::random().toHexString())),
      addrType_(addrType),
      mgmtd_(mgmtd) {}

BaseSelectionStrategy::~BaseSelectionStrategy() { mgmtd_->removeRoutingInfoListener(name_); }

void BaseSelectionStrategy::registerListener() {
  mgmtd_->addRoutingInfoListener(name_, [this](auto routing) { update(routing); });
  auto routing = mgmtd_->getRoutingInfo();
  if (!routing) {
    XLOGF(ERR, "meta::ServerSelection failed to get routing info from mgmtd!");
  } else {
    update(routing);
  }
}

void BaseSelectionStrategy::update(std::shared_ptr<RoutingInfo> routing) {
  auto rlock = servers_.rlock();
  if (rlock->routing != routing) {
    rlock.unlock();
    {
      auto wlock = servers_.wlock();
      auto updated = wlock->update(routing, addrType_);
      if (updated) {
        onUpdate(*wlock);
      }
    }
    rlock = servers_.rlock();
  }
}

Result<ServerSelectionStrategy::NodeInfo> BaseSelectionStrategy::select(const std::set<flat::NodeId> &skipNodes) {
  auto rlock = servers_.rlock();
  if (rlock->nodeList.empty()) {
    XLOGF(ERR, "meta::ServerSelection doesn't find available node.");
    return makeError(MgmtdClientCode::kMetaServiceNotAvailable);
  }

  if (!skipNodes.empty()) {
    auto nodes = skip(rlock->nodeList, skipNodes);
    if (!nodes.empty()) {
      auto nodeId = selectFrom(nodes, skipNodes);
      auto addr = rlock->nodeAddress.at(nodeId);
      auto hostname = rlock->routing->getNode(nodeId)->app.hostname;
      XLOGF(DBG, "meta::ServerSelection select {}, addr {}", nodeId, addr);
      return NodeInfo{nodeId, addr, hostname};
    }
    noMetaAfterSkip.addSample(1);
    XLOGF(WARN,
          "meta::SeverSelection have no available nodes after skip nodes {}.",
          fmt::join(skipNodes.begin(), skipNodes.end(), ","));
  }

  auto nodeId = selectFrom(rlock->nodeList, skipNodes);
  auto addr = rlock->nodeAddress.at(nodeId);
  auto hostname = rlock->routing->getNode(nodeId)->app.hostname;
  XLOGF(DBG, "meta::ServerSelection select {}, addr {}", nodeId, addr);
  return NodeInfo{nodeId, addr, hostname};
}

void BaseSelectionStrategy::onUpdate(ServerList &) { preferMetaServer.set(0); }

flat::NodeId RoundRobinServerSelection::selectFrom(const std::vector<flat::NodeId> &nodes,
                                                   const std::set<flat::NodeId> &) {
  assert(!nodes.empty());
  auto index = next_.fetch_add(1) % nodes.size();
  return nodes.at(index);
}

flat::NodeId UniformRandomServerSelection::selectFrom(const std::vector<flat::NodeId> &nodes,
                                                      const std::set<flat::NodeId> &) {
  assert(!nodes.empty());
  std::scoped_lock<std::mutex> lock(mutex_);
  auto index = dist_(gen_, index_range(0, nodes.size() - 1));
  return nodes.at(index);
}

flat::NodeId RandomFollowServerSelection::selectFrom(const std::vector<flat::NodeId> &nodes,
                                                     const std::set<flat::NodeId> &skipHint) {
  assert(!nodes.empty() && !prefer_.empty());
  for (auto node : prefer_) {
    if (!skipHint.contains(node)) {
      return node;
    }
  }

  auto index = folly::Random::rand32(nodes.size());
  return nodes[index];
}

void RandomFollowServerSelection::onUpdate(ServerList &servers) {
  prefer_ = servers.nodeList;
  std::sort(prefer_.begin(), prefer_.end(), [&](const auto &node1, const auto &node2) {
    return Weight::calculate(node1, token_) > Weight::calculate(node2, token_);
  });
  XLOGF(INFO, "RandomFollowServerSelection meta server order {}", fmt::join(prefer_.begin(), prefer_.end(), ","));

  assert(prefer_.size() == servers.nodeList.size());
  if (!prefer_.empty()) {
    auto prefer = *prefer_.begin();
    preferMetaServer.set(prefer.toUnderType());
    XLOGF(INFO,
          "RandomFollowServerSelection choose [{}, {}, {}] as preferred",
          prefer,
          servers.routing->getNode(prefer)->app.hostname,
          servers.nodeAddress[prefer]);
  }
}

}  // namespace hf3fs::meta::client
