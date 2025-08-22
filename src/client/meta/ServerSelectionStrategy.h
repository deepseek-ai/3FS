#pragma once

#include <fmt/format.h>
#include <folly/SharedMutex.h>
#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/mgmtd/RoutingInfo.h"
#include "common/app/NodeId.h"
#include "common/utils/Address.h"
#include "common/utils/MagicEnum.hpp"

namespace hf3fs::meta::client {

using hf3fs::client::ICommonMgmtdClient;
using hf3fs::client::RoutingInfo;

enum ServerSelectionMode {
  RoundRobin,
  UniformRandom,
  RandomFollow,
};

struct ServerSelectionStrategy : folly::NonCopyableNonMovable {
  struct NodeInfo {
    flat::NodeId nodeId;
    net::Address address;
    std::string hostname;
  };

  static std::unique_ptr<ServerSelectionStrategy> create(ServerSelectionMode mode,
                                                         std::shared_ptr<ICommonMgmtdClient> mgmtd,
                                                         net::Address::Type addrType);

  virtual ~ServerSelectionStrategy() = default;

  virtual ServerSelectionMode mode() = 0;
  virtual net::Address::Type addrType() = 0;
  virtual Result<NodeInfo> select(const std::set<flat::NodeId> &skipNodes) = 0;
  virtual std::optional<NodeInfo> get(flat::NodeId node) = 0;
};

struct ServerList {
  std::shared_ptr<RoutingInfo> routing;
  std::vector<flat::NodeId> nodeList;
  std::map<flat::NodeId, net::Address> nodeAddress;

  bool update(std::shared_ptr<RoutingInfo> newRouting, net::Address::Type addrType);
  std::optional<ServerSelectionStrategy::NodeInfo> get(flat::NodeId nodeId) const;
};

class BaseSelectionStrategy : public ServerSelectionStrategy {
 public:
  BaseSelectionStrategy(std::shared_ptr<ICommonMgmtdClient> mgmtd, net::Address::Type addrType);
  ~BaseSelectionStrategy() override;

  Result<NodeInfo> select(const std::set<flat::NodeId> &skipNodes) override;
  std::optional<NodeInfo> get(flat::NodeId node) override { return servers_.rlock()->get(node); }
  net::Address::Type addrType() override { return addrType_; }

  virtual flat::NodeId selectFrom(const std::vector<flat::NodeId> &nodes, const std::set<flat::NodeId> &skipHint) = 0;
  virtual void onUpdate(ServerList &);

 protected:
  // NOTE: must call this in subclass
  void registerListener();
  void update(std::shared_ptr<RoutingInfo> routing);

  std::string name_;
  net::Address::Type addrType_;
  std::shared_ptr<ICommonMgmtdClient> mgmtd_;
  folly::Synchronized<ServerList> servers_;
};

struct RoundRobinServerSelection : public BaseSelectionStrategy {
  RoundRobinServerSelection(std::shared_ptr<ICommonMgmtdClient> mgmtd, net::Address::Type addrType)
      : BaseSelectionStrategy(mgmtd, addrType) {
    registerListener();
  }
  ~RoundRobinServerSelection() override = default;

  ServerSelectionMode mode() override { return ServerSelectionMode::RoundRobin; }
  flat::NodeId selectFrom(const std::vector<flat::NodeId> &nodes, const std::set<flat::NodeId> &skipHint) override;

 private:
  std::atomic<size_t> next_{0};
};

struct UniformRandomServerSelection : public BaseSelectionStrategy {
  UniformRandomServerSelection(std::shared_ptr<ICommonMgmtdClient> mgmtd, net::Address::Type addrType)
      : BaseSelectionStrategy(mgmtd, addrType),
        mutex_(),
        gen_(std::random_device{}()) {
    registerListener();
  }
  ~UniformRandomServerSelection() override = default;

  ServerSelectionMode mode() override { return ServerSelectionMode::UniformRandom; }
  flat::NodeId selectFrom(const std::vector<flat::NodeId> &nodes, const std::set<flat::NodeId> &skipHint) override;

 private:
  using index_range = std::uniform_int_distribution<size_t>::param_type;
  std::mutex mutex_;
  std::mt19937_64 gen_;
  std::uniform_int_distribution<size_t> dist_;
};

struct RandomFollowServerSelection : public BaseSelectionStrategy {
  RandomFollowServerSelection(std::shared_ptr<ICommonMgmtdClient> mgmtd, net::Address::Type addrType)
      : BaseSelectionStrategy(mgmtd, addrType),
        token_(Uuid::random()) {
    registerListener();
  }
  ~RandomFollowServerSelection() override = default;

  ServerSelectionMode mode() override { return ServerSelectionMode::RandomFollow; }
  flat::NodeId selectFrom(const std::vector<flat::NodeId> &nodes, const std::set<flat::NodeId> &skipHint) override;
  std::optional<NodeInfo> get(flat::NodeId node) override { return servers_.rlock()->get(node); }
  void onUpdate(ServerList &servers) override;

 private:
  Uuid token_;
  std::vector<flat::NodeId> prefer_;
};
}  // namespace hf3fs::meta::client

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::meta::client::ServerSelectionStrategy::NodeInfo> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::meta::client::ServerSelectionStrategy::NodeInfo &node, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "[{}]@{}.{}", node.address, node.nodeId, node.hostname);
  }
};

FMT_END_NAMESPACE
