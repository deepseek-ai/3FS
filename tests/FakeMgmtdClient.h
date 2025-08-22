#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <folly/concurrency/AtomicSharedPtr.h>
#include <memory>
#include <set>
#include <vector>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/mgmtd/RoutingInfo.h"
#include "common/app/ClientId.h"
#include "common/utils/Address.h"
#include "common/utils/UtcTime.h"
#include "fbs/mgmtd/ChainRef.h"
#include "fbs/mgmtd/ChainTargetInfo.h"
#include "fbs/mgmtd/ClientSession.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "fbs/mgmtd/RoutingInfo.h"
#include "fbs/mgmtd/Rpc.h"
#include "fbs/mgmtd/TargetInfo.h"

namespace hf3fs::tests {
class FakeMgmtdClient : public hf3fs::client::ICommonMgmtdClient {
 public:
  static std::shared_ptr<FakeMgmtdClient> create(
      std::vector<std::tuple<flat::ChainTableId, size_t, size_t>> tables = {},
      size_t numMetas = 1,
      size_t numStorages = 1) {
    auto mgmtd = std::make_shared<FakeMgmtdClient>();
    for (auto [table, numChains, numReplica] : tables) {
      mgmtd->addChainTable(table, numChains, numReplica);
    }
    mgmtd->addNodes(numMetas, numStorages);
    for (size_t i = 0; i < 10; i++) {
      mgmtd->addClient(Uuid::random());
    }
    return mgmtd;
  }

  FakeMgmtdClient()
      : routingInfo_(std::make_shared<hf3fs::client::RoutingInfo>(std::make_shared<hf3fs::flat::RoutingInfo>(),
                                                                  SteadyClock::now())) {}

  std::shared_ptr<hf3fs::client::RoutingInfo> getRoutingInfo() override { return routingInfo_.load(); }

  CoTryTask<void> refreshRoutingInfo(bool) override { co_return Void{}; }

  bool addRoutingInfoListener(String name, RoutingInfoListener func) override {
    listeners_[name] = func;
    return true;
  }
  bool removeRoutingInfoListener(std::string_view name) override {
    listeners_.erase(std::string(name));
    return true;
  }

  CoTryTask<mgmtd::ListClientSessionsRsp> listClientSessions() override {
    auto guard = sessions_.lock();
    mgmtd::ListClientSessionsRsp rsp;
    rsp.bootstrapping = false;
    rsp.sessions = *guard;
    for (auto &session : rsp.sessions) {
      session.lastExtend = UtcClock::now();
    }
    co_return rsp;
  }

  std::set<Uuid> getActiveClients() {
    std::set<Uuid> active;
    auto guard = sessions_.lock();
    for (const auto &client : *guard) {
      active.insert(Uuid::fromHexString(client.clientId).value());
    }
    return active;
  }

  ClientId getOneActiveClient() {
    auto guard = sessions_.lock();
    return ClientId(Uuid::fromHexString(guard->at(folly::Random::rand32(guard->size())).clientId).value());
  }

  void clearActiveClient() { sessions_.lock()->clear(); }

  void addClient(Uuid client) {
    flat::ClientSession session;
    session.clientId = client.toHexString();
    sessions_.lock()->push_back(session);
  }

  CoTryTask<mgmtd::GetClientSessionRsp> getClientSession(const String &) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<std::optional<flat::ConfigInfo>> getConfig(flat::NodeType, flat::ConfigVersion) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<RHStringHashMap<flat::ConfigVersion>> getConfigVersions() override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  CoTryTask<std::vector<flat::TagPair>> getUniversalTags(const String &universalId) override {
    co_return makeError(StatusCode::kNotImplemented);
  }

  std::shared_ptr<hf3fs::client::RoutingInfo> cloneRoutingInfo() {
    auto curr = routingInfo_.load();
    auto next = std::make_shared<hf3fs::client::RoutingInfo>(std::make_shared<hf3fs::flat::RoutingInfo>(*curr->raw()),
                                                             SteadyClock::now());
    next->raw()->routingInfoVersion++;
    return next;
  }

  void setRoutingInfo(std::shared_ptr<hf3fs::client::RoutingInfo> next) {
    routingInfo_.store(next);
    for (auto &[name, func] : listeners_) {
      func(routingInfo_.load());
    }
  }

  void addChainTable(flat::ChainTableId tableId, size_t numChains, size_t numReplica = 1) {
    auto next = cloneRoutingInfo();
    SCOPE_EXIT { setRoutingInfo(next); };
    auto &routing = next->raw();
    if (!routing->chainTables.contains(tableId)) {
      routing->chainTables[tableId] = {};
    }
    auto version = flat::ChainTableVersion(routing->chainTables[tableId].size() + 1);
    auto &table = routing->chainTables[tableId][version];
    table.chainTableVersion = version;
    for (size_t index = 1; index <= numChains; index++) {
      flat::ChainInfo chain;
      chain.chainId = flat::ChainId(tableId * 100000 + index);
      chain.chainVersion = flat::ChainVersion(1);
      chain.targets = {};
      for (size_t replica = 0; replica < numReplica; replica++) {
        flat::ChainTargetInfo target;
        target.targetId = flat::TargetId(folly::Random::rand32());
        target.publicState = flat::PublicTargetState::SERVING;
        chain.targets.push_back(target);
      }
      routing->chains[chain.chainId] = chain;
      table.chains.push_back(chain.chainId);
    }
  }

  void clearNodes() {
    auto next = cloneRoutingInfo();
    SCOPE_EXIT { setRoutingInfo(next); };
    next->raw()->nodes.clear();
  }

  void addNodes(size_t numMetas, size_t numStorages) {
    auto next = cloneRoutingInfo();
    SCOPE_EXIT { setRoutingInfo(next); };
    auto &routing = next->raw();
    for (size_t i = 0; i < numMetas; i++) {
      auto id = metaId_++;
      flat::NodeInfo info;
      info.app.nodeId = flat::NodeId(id);
      info.app.hostname = fmt::format("host-{}", id);
      info.type = flat::NodeType::META;
      info.status = flat::NodeStatus::HEARTBEAT_CONNECTED;
      info.app.serviceGroups.emplace_back(
          std::set<String>{"MetaSerde"},
          std::vector<net::Address>{net::Address::from(fmt::format("RDMA://127.0.0.1:{}", id)).value(),
                                    net::Address::from(fmt::format("TCP://127.0.0.1:{}", id)).value()});
      routing->nodes[flat::NodeId(id)] = info;
    }
    for (size_t i = 0; i < numStorages; i++) {
      auto id = storageId_++;
      flat::NodeInfo info;
      info.app.nodeId = flat::NodeId(id);
      info.app.hostname = fmt::format("host-{}", id);
      info.type = flat::NodeType::STORAGE;
      info.status = flat::NodeStatus::HEARTBEAT_CONNECTED;
      info.app.serviceGroups.emplace_back(
          std::set<String>{"Storage"},
          std::vector<net::Address>{net::Address::from(fmt::format("RDMA://127.0.0.1:{}", id)).value(),
                                    net::Address::from(fmt::format("TCP://127.0.0.1:{}", id)).value()});
      routing->nodes[flat::NodeId(id)] = info;
    }
  }

 private:
  folly::Synchronized<std::vector<flat::ClientSession>, std::mutex> sessions_;
  folly::atomic_shared_ptr<client::RoutingInfo> routingInfo_;
  std::map<std::string, RoutingInfoListener> listeners_;
  size_t metaId_ = 50;
  size_t storageId_ = 10000;
};

}  // namespace hf3fs::tests
