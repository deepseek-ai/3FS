#include <folly/experimental/coro/BlockingWait.h>

#include "MgmtdTestHelper.h"
#include "common/kv/KeyPrefix.h"
#include "common/kv/mem/MemKVEngine.h"
#include "mgmtd/ops/Include.h"
#include "mgmtd/service/MgmtdOperator.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::mgmtd::testing {
class MgmtdOperatorTest : public ::testing::Test {
 protected:
  MgmtdOperatorTest() {
    flat::AppInfo info;
    info.nodeId = flat::NodeId(10000);
    info.clusterId = "clusterId";
    info.hostname = "hostname0";
    info.pid = 100;

    flat::ServiceGroupInfo sgi0;
    sgi0.services.insert("MgmtdService");
    sgi0.endpoints.push_back(net::Address::fromString("127.0.0.1:9999"));
    info.serviceGroups.push_back(std::move(sgi0));

    defaultConfig_.set_lease_length(1_ms);
    defaultConfig_.set_extend_lease_interval(0_s);
    defaultConfig_.set_suspicious_lease_interval(100_us);
    defaultConfig_.set_heartbeat_timestamp_valid_window(1_ms);
    defaultConfig_.set_allow_heartbeat_from_unregistered(false);

    env_ = std::make_shared<core::ServerEnv>();
    env_->setAppInfo(info);
    env_->setKvEngine(std::make_shared<kv::MemKVEngine>());
    env_->setUtcTimeGenerator(generator_);
    env_->setBackgroundExecutor(&executor_);
  }

  flat::HeartbeatInfo from(flat::NodeId id) {
    flat::HeartbeatInfo info;
    info.app.nodeId = id;
    return info;
  }

  flat::HeartbeatInfo from(flat::NodeId id, flat::HeartbeatInfo info) {
    info.app.nodeId = id;
    return info;
  }

  CoTryTask<void> setPrimary(MgmtdOperator &mgmtd, flat::NodeId primaryId, UtcTime now) {
    MgmtdStore store;
    flat::PersistentNodeInfo info;
    info.nodeId = primaryId;

    MgmtdTestHelper helper(mgmtd);
    now_ = now;
    auto txn = helper.getEngine().createReadWriteTransaction();
    auto result = co_await store.extendLease(*txn, info, std::chrono::microseconds(1000), now);
    CO_RETURN_ON_ERROR(result);
    co_return co_await txn->commit();
  }

  CoTryTask<mgmtd::GetPrimaryMgmtdRsp> getPrimaryMgmtd(MgmtdOperator &mgmtd, const String &clusterId) {
    co_return co_await mgmtd.getPrimaryMgmtd(mgmtd::GetPrimaryMgmtdReq::create(clusterId), {});
  }

  CoTryTask<mgmtd::GetRoutingInfoRsp> getRoutingInfo(MgmtdOperator &mgmtd,
                                                     const String &clusterId,
                                                     flat::RoutingInfoVersion version) {
    co_return co_await mgmtd.getRoutingInfo(mgmtd::GetRoutingInfoReq::create(clusterId, version), {});
  }

  CoTryTask<mgmtd::HeartbeatRsp> heartbeat(MgmtdOperator &mgmtd,
                                           const String &clusterId,
                                           const flat::HeartbeatInfo &hb,
                                           UtcTime timestamp) {
    co_return co_await mgmtd.heartbeat(mgmtd::HeartbeatReq::create(clusterId, hb, timestamp), {});
  }

  CoTryTask<mgmtd::EnableNodeRsp> enableNode(MgmtdOperator &mgmtd, const String &clusterId, flat::NodeId nodeId) {
    co_return co_await mgmtd.enableNode(mgmtd::EnableNodeReq::create(clusterId, nodeId), {});
  }

  CoTryTask<mgmtd::DisableNodeRsp> disableNode(MgmtdOperator &mgmtd, const String &clusterId, flat::NodeId nodeId) {
    co_return co_await mgmtd.disableNode(mgmtd::DisableNodeReq::create(clusterId, nodeId), {});
  }

  CoTryTask<mgmtd::RegisterNodeRsp> registerNode(MgmtdOperator &mgmtd,
                                                 const String &clusterId,
                                                 flat::NodeId nodeId,
                                                 flat::NodeType type) {
    co_return co_await mgmtd.registerNode(mgmtd::RegisterNodeReq::create(clusterId, nodeId, type), {});
  }

  CoTryTask<mgmtd::SetConfigRsp> setConfig(MgmtdOperator &mgmtd,
                                           const String &clusterId,
                                           flat::NodeType nodeType,
                                           const String &content) {
    co_return co_await mgmtd.setConfig(mgmtd::SetConfigReq::create(clusterId, nodeType, content), {});
  }

  CoTryTask<mgmtd::GetConfigRsp> getConfig(MgmtdOperator &mgmtd,
                                           const String &clusterId,
                                           flat::NodeType nodeType,
                                           flat::ConfigVersion version) {
    co_return co_await mgmtd.getConfig(mgmtd::GetConfigReq::create(clusterId, nodeType, version), {});
  }

  CoTryTask<mgmtd::SetChainTableRsp> setChainTable(MgmtdOperator &mgmtd,
                                                   const String &clusterId,
                                                   flat::ChainTableId tableId,
                                                   const std::vector<flat::ChainId> &chains) {
    co_return co_await mgmtd.setChainTable(mgmtd::SetChainTableReq::create(clusterId, tableId, chains), {});
  }

  CoTryTask<mgmtd::ExtendClientSessionRsp> extendClientSession(MgmtdOperator &mgmtd, auto &&...args) {
    co_return co_await mgmtd.extendClientSession(
        mgmtd::ExtendClientSessionReq::create(std::forward<decltype(args)>(args)...),
        {});
  }

  CoTryTask<mgmtd::ListClientSessionsRsp> listClientSessions(MgmtdOperator &mgmtd, const String &clusterId) {
    co_return co_await mgmtd.listClientSessions(mgmtd::ListClientSessionsReq::create(clusterId), {});
  }

  CoTryTask<mgmtd::SetNodeTagsRsp> setNodeTags(MgmtdOperator &mgmtd,
                                               const String &clusterId,
                                               flat::NodeId nodeId,
                                               std::vector<flat::TagPair> tags,
                                               flat::SetTagMode mode) {
    co_return co_await mgmtd.setNodeTags(mgmtd::SetNodeTagsReq::create(clusterId, nodeId, std::move(tags), mode), {});
  }

  UtcTime now_ = UtcTime::fromMicroseconds(10000);
  std::function<UtcTime()> generator_ = [this] { return now_; };
  MgmtdConfig defaultConfig_;
  std::shared_ptr<core::ServerEnv> env_;
  CPUExecutorGroup executor_{4, "TEST"};
};

TEST_F(MgmtdOperatorTest, testGetPrimaryMgmtd) {
  MgmtdOperator mgmtd(env_, defaultConfig_);
  folly::coro::blockingWait([&]() -> CoTask<void> {
    // wrong clusterid
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kClusterIdMismatch, getPrimaryMgmtd(mgmtd, "wrong"));

    // no lease holder
    {
      auto ret = co_await getPrimaryMgmtd(mgmtd, "clusterId");
      CO_ASSERT_OK(ret);
      CO_ASSERT_TRUE(!ret->primary.has_value());
    }

    // node1 got lease
    CO_AWAIT_ASSERT_OK(setPrimary(mgmtd, flat::NodeId(1), UtcTime::fromMicroseconds(10000)));

    // node0 doesn't know node1's lease, load from store
    {
      auto ret = co_await getPrimaryMgmtd(mgmtd, "clusterId");
      CO_ASSERT_OK(ret);
      CO_ASSERT_TRUE(ret->primary.has_value());
      CO_ASSERT_EQ(ret->primary->nodeId, flat::NodeId(1));
    }

    // node0 has known node1's lease, directly return
    {
      MgmtdTestHelper helper(mgmtd);
      co_await helper.extendLease();
      // hack: erase lease from engine to ensure the result come from cached lease
      auto txn = helper.getEngine().createReadWriteTransaction();
      auto key = fmt::format("{}MgmtdLease", kv::toStr(kv::KeyPrefix::Single));
      CO_AWAIT_ASSERT_OK(txn->clear(key));
      CO_AWAIT_ASSERT_OK(txn->commit());

      auto ret = co_await getPrimaryMgmtd(mgmtd, "clusterId");
      CO_ASSERT_OK(ret);
      CO_ASSERT_TRUE(ret->primary.has_value());
      CO_ASSERT_EQ(ret->primary->nodeId, flat::NodeId(1));
    }

    // node1's lease isn't trustable
    {
      // node2 got lease
      CO_AWAIT_ASSERT_OK(setPrimary(mgmtd, flat::NodeId(2), UtcTime::fromMicroseconds(11001)));
      now_ = UtcTime::fromMicroseconds(10950);

      auto ret = co_await getPrimaryMgmtd(mgmtd, "clusterId");
      CO_ASSERT_OK(ret);
      CO_ASSERT_TRUE(ret->primary.has_value());
      CO_ASSERT_EQ(ret->primary->nodeId, flat::NodeId(2));
    }
  }());
}

TEST_F(MgmtdOperatorTest, testRegisterNode) {
  MgmtdOperator mgmtd(env_, defaultConfig_);
  folly::coro::blockingWait([&]() -> CoTask<void> {
    // wrong clusterid
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kClusterIdMismatch,
                          registerNode(mgmtd, "wrong", flat::NodeId(1), flat::NodeType::MGMTD));

    // not primary: no primary right now
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kNotPrimary,
                          registerNode(mgmtd, "clusterId", flat::NodeId(1), flat::NodeType::MGMTD));

    // not primary: not self
    CO_AWAIT_ASSERT_OK(setPrimary(mgmtd, flat::NodeId(1), UtcTime::fromMicroseconds(10000)));
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kNotPrimary,
                          registerNode(mgmtd, "clusterId", flat::NodeId(2), flat::NodeType::MGMTD));

    // not primary: cached lease isn't trustable
    {
      now_ = UtcTime::fromMicroseconds(10950);
      CO_AWAIT_ASSERT_ERROR(MgmtdCode::kNotPrimary,
                            registerNode(mgmtd, "clusterId", flat::NodeId(2), flat::NodeType::MGMTD));
    }

    // succeed: self holds un-suspicious lease
    {
      now_ = UtcTime::fromMicroseconds(12000);
      co_await MgmtdTestHelper(mgmtd).extendLease();

      CO_AWAIT_ASSERT_OK(registerNode(mgmtd, "clusterId", flat::NodeId(1), flat::NodeType::MGMTD));

      auto ret = co_await getRoutingInfo(mgmtd, "clusterId", flat::RoutingInfoVersion(0));
      CO_ASSERT_OK(ret);
      CO_ASSERT_TRUE(ret->info.has_value());
      CO_ASSERT_TRUE(ret->info->nodes.contains(flat::NodeId(1)));
      const auto &node = ret->info->nodes[flat::NodeId(1)];
      CO_ASSERT_EQ(node.app.nodeId, flat::NodeId(1));
      CO_ASSERT_EQ(node.type, flat::NodeType::MGMTD);
      CO_ASSERT_EQ(node.status, flat::NodeStatus::HEARTBEAT_CONNECTING);
    }

    // duplicated
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kRegisterFail,
                          registerNode(mgmtd, "clusterId", flat::NodeId(1), flat::NodeType::MGMTD));

    // not primary: self lease is suspicious
    {
      now_ = UtcTime::fromMicroseconds(12950);
      CO_AWAIT_ASSERT_ERROR(MgmtdCode::kNotPrimary,
                            registerNode(mgmtd, "clusterId", flat::NodeId(2), flat::NodeType::MGMTD));
    }
  }());
}

TEST_F(MgmtdOperatorTest, testGetRoutingInfo) {
  MgmtdOperator mgmtd(env_, defaultConfig_);
  folly::coro::blockingWait([&]() -> CoTask<void> {
    // wrong clusterid
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kClusterIdMismatch, getRoutingInfo(mgmtd, "wrong", flat::RoutingInfoVersion(0)));

    // not primary: no primary right now
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kNotPrimary, getRoutingInfo(mgmtd, "clusterId", flat::RoutingInfoVersion(1)));

    // init
    co_await MgmtdTestHelper(mgmtd).extendLease();
    {
      auto ret = co_await getRoutingInfo(mgmtd, "clusterId", flat::RoutingInfoVersion(0));
      CO_ASSERT_OK(ret);
      CO_ASSERT_TRUE(ret->info.has_value());
      CO_ASSERT_EQ(ret->info->routingInfoVersion, flat::RoutingInfoVersion(1));
      CO_ASSERT_EQ(ret->info->nodes.size(), 1);
      CO_ASSERT_TRUE(ret->info->nodes.contains(flat::NodeId(10000)));  // self
      const auto &node = ret->info->nodes[flat::NodeId(10000)];
      CO_ASSERT_EQ(node.app.nodeId, flat::NodeId(10000));
      CO_ASSERT_EQ(node.type, flat::NodeType::MGMTD);
      CO_ASSERT_EQ(node.status, flat::NodeStatus::PRIMARY_MGMTD);
    }

    // return nullopt when version is up-to-date
    {
      auto ret = co_await getRoutingInfo(mgmtd, "clusterId", flat::RoutingInfoVersion(1));
      CO_ASSERT_OK(ret);
      CO_ASSERT_TRUE(!ret->info.has_value());
    }

    // return error when version is newer than server's
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kInvalidRoutingInfoVersion,
                          getRoutingInfo(mgmtd, "clusterId", flat::RoutingInfoVersion(2)));
  }());
}

TEST_F(MgmtdOperatorTest, testHeartbeat) {
  MgmtdOperator mgmtd(env_, defaultConfig_);
  folly::coro::blockingWait([&]() -> CoTask<void> {
    // wrong clusterid
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kClusterIdMismatch, heartbeat(mgmtd, "wrong", from(flat::NodeId{1}), now_));

    flat::HeartbeatInfo info1;
    {
      info1.app.nodeId = flat::NodeId(1);
      info1.app.hostname = "hostname1";
      info1.app.pid = 101;

      flat::ServiceGroupInfo sgi0;
      sgi0.services.emplace("MgmtdService");
      sgi0.endpoints.emplace_back(net::Address::fromString("127.0.0.1:9876"));

      info1.app.serviceGroups.push_back(sgi0);
      info1.set(flat::MgmtdHeartbeatInfo{});
      info1.hbVersion = flat::HeartbeatVersion{1};
    }
    // not primary
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kNotPrimary, heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info1), now_));

    co_await MgmtdTestHelper(mgmtd).extendLease();

    // forbid self heartbeat
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kHeartbeatFail, heartbeat(mgmtd, "clusterId", from(flat::NodeId(10000)), now_));

    // timestamp deviation
    CO_AWAIT_ASSERT_ERROR(
        MgmtdCode::kHeartbeatFail,
        heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info1), now_ + std::chrono::microseconds(2500)));

    // unregistered
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kHeartbeatFail, heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info1), now_));

    CO_AWAIT_ASSERT_OK(registerNode(mgmtd, "clusterId", flat::NodeId(1), flat::NodeType::MGMTD));

    // type mismatch with registered
    {
      auto info2 = info1;
      info2.set(flat::MetaHeartbeatInfo{});
      CO_AWAIT_ASSERT_ERROR(MgmtdCode::kHeartbeatFail,
                            heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info2), now_));
    }

    // first successful heartbeat, status changed to CONNECTED
    {
      now_ = UtcTime::fromMicroseconds(10500);
      CO_AWAIT_ASSERT_OK(heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info1), now_));
      auto node = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));
      CO_ASSERT_EQ(node.base().type, flat::NodeType::MGMTD);
      CO_ASSERT_EQ(node.base().status, flat::NodeStatus::HEARTBEAT_CONNECTED);
      CO_ASSERT_EQ(node.base().app, info1.app) << fmt::format("node.app = {} info1.app = {}",
                                                              serde::toJsonString(node.base().app),
                                                              serde::toJsonString(info1.app));
      CO_ASSERT_EQ(node.base().lastHeartbeatTs, now_);
    }

    // extend heartbeat: nothing changed
    {
      info1.hbVersion = flat::HeartbeatVersion{3};
      auto node1 = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));
      now_ = UtcTime::fromMicroseconds(10501);
      CO_AWAIT_ASSERT_OK(heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info1), now_));
      auto node2 = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));
      CO_ASSERT_EQ(node2.base().lastHeartbeatTs, now_);
      // only lastHeartbeatTs differs
      node2.base().lastHeartbeatTs = node1.base().lastHeartbeatTs;
      CO_ASSERT_EQ(node1.base(), node2.base());
    }

    // stale heartbeat version
    {
      info1.hbVersion = flat::HeartbeatVersion{2};
      now_ = UtcTime::fromMicroseconds(10502);
      CO_AWAIT_ASSERT_ERROR(MgmtdCode::kHeartbeatVersionStale,
                            heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info1), now_));
    }

    // service info changed without restarting
    {
      auto info2 = info1;
      info2.hbVersion = flat::HeartbeatVersion{4};
      info2.app.serviceGroups[0].services.emplace("OtherService");
      CO_AWAIT_ASSERT_OK(heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info2), now_));

      auto node = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));
      CO_ASSERT_EQ(node.base().app, info2.app);
    }

    // restart: lastHeartbeatTs changed
    {
      auto node1 = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));

      auto info2 = info1;
      info2.app.hostname = "hostname0-2";
      info2.hbVersion = flat::HeartbeatVersion{1};

      now_ = UtcTime::fromMicroseconds(10502);
      CO_AWAIT_ASSERT_ERROR(MgmtdCode::kHeartbeatVersionStale,
                            heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info2), now_));

      info2.hbVersion = flat::HeartbeatVersion{5};
      CO_AWAIT_ASSERT_OK(heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info2), now_));

      auto node2 = co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1));

      CO_ASSERT_EQ(node2->base().type, node1.base().type);
      CO_ASSERT_EQ(node2->base().status, node1.base().status);
      CO_ASSERT_EQ(node2->base().app, info2.app);
      CO_ASSERT_EQ(node2->base().lastHeartbeatTs, now_);

      info2.app.pid = 102;
      info2.hbVersion = flat::HeartbeatVersion{6};
      now_ = UtcTime::fromMicroseconds(10503);
      CO_AWAIT_ASSERT_OK(heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info2), now_));

      node2 = co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1));

      CO_ASSERT_EQ(node2->base().type, node1.base().type);
      CO_ASSERT_EQ(node2->base().status, node1.base().status);
      CO_ASSERT_EQ(node2->base().app, info2.app);
      CO_ASSERT_EQ(node2->base().lastHeartbeatTs, now_);
    }

    // reconnected
    {
      auto &innerNode = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));
      innerNode.base().status = flat::NodeStatus::HEARTBEAT_FAILED;

      now_ = UtcTime::fromMicroseconds(10504);
      info1.hbVersion = flat::HeartbeatVersion{7};
      CO_AWAIT_ASSERT_OK(heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info1), now_));

      auto node = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));
      CO_ASSERT_EQ(node.base().status, flat::NodeStatus::HEARTBEAT_CONNECTED);
      CO_ASSERT_EQ(node.base().lastHeartbeatTs, now_);
    }

    // reject heartbeats from offlined nodes
    {
      // TODO: migrate to a formal method
      auto &innerNode = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));
      innerNode.base().status = flat::NodeStatus::DISABLED;

      CO_AWAIT_ASSERT_ERROR(MgmtdCode::kHeartbeatFail,
                            heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), info1), now_));
    }

    // allow timestamp deviation and unregistered
    {
      auto options = defaultConfig_.clone();
      options.set_heartbeat_timestamp_valid_window(0_ms);
      options.set_allow_heartbeat_from_unregistered(true);
      auto env = env_;
      env->setKvEngine(std::make_shared<kv::MemKVEngine>());
      MgmtdOperator mgmtd1(std::move(env), options);

      now_ = UtcTime::fromMicroseconds(10000);
      co_await MgmtdTestHelper(mgmtd1).extendLease();

      CO_AWAIT_ASSERT_OK(
          heartbeat(mgmtd1, "clusterId", from(flat::NodeId(1), info1), UtcTime::fromMicroseconds(12500)));
      auto node = *(co_await MgmtdTestHelper(mgmtd1).getNodeInfo(flat::NodeId(1)));
      CO_ASSERT_EQ(node.base().type, flat::NodeType::MGMTD);
      CO_ASSERT_EQ(node.base().status, flat::NodeStatus::HEARTBEAT_CONNECTED);
      CO_ASSERT_EQ(node.base().app, info1.app);
      CO_ASSERT_EQ(node.base().lastHeartbeatTs, now_);
    }
  }());
}

TEST_F(MgmtdOperatorTest, testRecovery) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto mgmtd1 = std::make_unique<MgmtdOperator>(env_, defaultConfig_);

    co_await MgmtdTestHelper(*mgmtd1).extendLease();

    flat::HeartbeatInfo info1;
    {
      info1.app.hostname = "hostname1";
      info1.app.pid = 101;

      flat::ServiceGroupInfo sgi0;
      sgi0.services.emplace("MgmtdService");
      sgi0.endpoints.emplace_back(net::Address::fromString("127.0.0.1:9876"));

      info1.app.serviceGroups.push_back(sgi0);
      info1.set(flat::MgmtdHeartbeatInfo{});
    }

    auto info2 = info1;
    info2.set(flat::MetaHeartbeatInfo{});

    CO_AWAIT_ASSERT_OK(registerNode(*mgmtd1, "clusterId", flat::NodeId(1), flat::NodeType::MGMTD));
    CO_AWAIT_ASSERT_OK(registerNode(*mgmtd1, "clusterId", flat::NodeId(2), flat::NodeType::META));
    CO_AWAIT_ASSERT_OK(registerNode(*mgmtd1, "clusterId", flat::NodeId(3), flat::NodeType::STORAGE));

    now_ += std::chrono::microseconds(10);
    CO_AWAIT_ASSERT_OK(heartbeat(*mgmtd1, "clusterId", from(flat::NodeId(1), info1), now_));
    CO_AWAIT_ASSERT_OK(heartbeat(*mgmtd1, "clusterId", from(flat::NodeId(2), info2), now_));

    co_await disableNode(*mgmtd1, "clusterId", flat::NodeId(2));

    mgmtd1.reset();
    mgmtd1 = std::make_unique<MgmtdOperator>(env_, defaultConfig_);

    co_await MgmtdTestHelper(*mgmtd1).extendLease();

    auto node = co_await MgmtdTestHelper(*mgmtd1).getNodeInfo(flat::NodeId(1));
    CO_ASSERT_EQ(node->base().status, flat::NodeStatus::HEARTBEAT_CONNECTING);

    node = co_await MgmtdTestHelper(*mgmtd1).getNodeInfo(flat::NodeId(2));
    CO_ASSERT_EQ(node->base().status, flat::NodeStatus::DISABLED);

    node = co_await MgmtdTestHelper(*mgmtd1).getNodeInfo(flat::NodeId(3));
    CO_ASSERT_EQ(node->base().status, flat::NodeStatus::HEARTBEAT_CONNECTING);
  }());
}

TEST_F(MgmtdOperatorTest, testDisable) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MgmtdOperator mgmtd(env_, defaultConfig_);
    // wrong clusterid
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kClusterIdMismatch, disableNode(mgmtd, "wrong", flat::NodeId(1)));
    // offline primary mgmtd
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg, disableNode(mgmtd, "clusterId", flat::NodeId(10000)));
    // not primary
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kNotPrimary, disableNode(mgmtd, "clusterId", flat::NodeId(1)));

    co_await MgmtdTestHelper(mgmtd).extendLease();
    // not registered
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kNodeNotFound, disableNode(mgmtd, "clusterId", flat::NodeId(1)));

    CO_AWAIT_ASSERT_OK(registerNode(mgmtd, "clusterId", flat::NodeId(1), flat::NodeType::MGMTD));
    CO_AWAIT_ASSERT_OK(disableNode(mgmtd, "clusterId", flat::NodeId(1)));

    auto info1 = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));
    CO_ASSERT_EQ(info1.base().status, flat::NodeStatus::DISABLED);

    // duplicated offline has no effect
    CO_AWAIT_ASSERT_OK(disableNode(mgmtd, "clusterId", flat::NodeId(1)));
    auto info2 = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));
    CO_ASSERT_EQ(info1.base(), info2.base());
  }());
}

TEST_F(MgmtdOperatorTest, testEnable) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MgmtdOperator mgmtd(env_, defaultConfig_);
    // wrong clusterid
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kClusterIdMismatch, enableNode(mgmtd, "wrong", flat::NodeId(1)));
    // offline primary mgmtd
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg, enableNode(mgmtd, "clusterId", flat::NodeId(10000)));
    // not primaryA
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kNotPrimary, enableNode(mgmtd, "clusterId", flat::NodeId(1)));

    co_await MgmtdTestHelper(mgmtd).extendLease();
    // not registered
    CO_AWAIT_ASSERT_ERROR(MgmtdCode::kNodeNotFound, enableNode(mgmtd, "clusterId", flat::NodeId(1)));

    CO_AWAIT_ASSERT_OK(registerNode(mgmtd, "clusterId", flat::NodeId(1), flat::NodeType::MGMTD));

    flat::HeartbeatInfo hb;
    hb.set(flat::MgmtdHeartbeatInfo{});
    CO_AWAIT_ASSERT_OK(heartbeat(mgmtd, "clusterId", from(flat::NodeId(1), hb), now_));

    auto info1 = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));
    CO_ASSERT_EQ(info1.base().status, flat::NodeStatus::HEARTBEAT_CONNECTED);

    // online an online node has no effect
    CO_AWAIT_ASSERT_OK(enableNode(mgmtd, "clusterId", flat::NodeId(1)));
    auto info2 = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));
    CO_ASSERT_EQ(info1.base(), info2.base());

    CO_AWAIT_ASSERT_OK(disableNode(mgmtd, "clusterId", flat::NodeId(1)));
    CO_AWAIT_ASSERT_OK(enableNode(mgmtd, "clusterId", flat::NodeId(1)));

    auto info3 = *(co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)));
    CO_ASSERT_EQ(info3.base().status, flat::NodeStatus::HEARTBEAT_CONNECTING);
  }());
}

TEST_F(MgmtdOperatorTest, testSetNodeTags) {
  auto f = [&]() -> CoTask<void> {
    MgmtdOperator mgmtd(env_, defaultConfig_);
    co_await MgmtdTestHelper(mgmtd).extendLease();

    CO_AWAIT_ASSERT_OK(registerNode(mgmtd, "clusterId", flat::NodeId(1), flat::NodeType::MGMTD));

    auto info1 = (co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)))->base();
    CO_ASSERT_TRUE(info1.tags.empty());

    auto setRes = co_await setNodeTags(mgmtd,
                                       "clusterId",
                                       flat::NodeId(1),
                                       {flat::TagPair("A", "1"), flat::TagPair("B", "2"), flat::TagPair("C")},
                                       flat::SetTagMode::UPSERT);
    CO_ASSERT_OK(setRes);

    auto expected = std::vector{flat::TagPair("A", "1"), flat::TagPair("B", "2"), flat::TagPair("C")};
    info1 = (co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)))->base();
    CO_ASSERT_EQ(setRes->info, info1);
    CO_ASSERT_EQ(info1.tags, expected);

    setRes = co_await setNodeTags(mgmtd,
                                  "clusterId",
                                  flat::NodeId(1),
                                  {flat::TagPair("B", "3"), flat::TagPair("D", "4")},
                                  flat::SetTagMode::UPSERT);
    CO_ASSERT_OK(setRes);

    expected =
        std::vector{flat::TagPair("A", "1"), flat::TagPair("B", "3"), flat::TagPair("C"), flat::TagPair("D", "4")};
    info1 = (co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)))->base();
    CO_ASSERT_EQ(setRes->info, info1);
    CO_ASSERT_EQ(info1.tags, expected) << fmt::format("actual: \n{}\nexpectd: {}",
                                                      serde::toJsonString(info1.tags),
                                                      serde::toJsonString(expected));

    setRes = co_await setNodeTags(mgmtd,
                                  "clusterId",
                                  flat::NodeId(1),
                                  {flat::TagPair("A", "2"), flat::TagPair("C", "3")},
                                  flat::SetTagMode::REPLACE);
    CO_ASSERT_OK(setRes);

    expected = std::vector{flat::TagPair("A", "2"), flat::TagPair("C", "3")};
    info1 = (co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)))->base();
    CO_ASSERT_EQ(setRes->info, info1);
    CO_ASSERT_EQ(info1.tags, expected);

    setRes =
        co_await setNodeTags(mgmtd, "clusterId", flat::NodeId(1), {flat::TagPair("A", "1")}, flat::SetTagMode::REMOVE);
    CO_ASSERT_ERROR(setRes, MgmtdCode::kInvalidTag);

    setRes = co_await setNodeTags(mgmtd, "clusterId", flat::NodeId(1), {flat::TagPair("A")}, flat::SetTagMode::REMOVE);

    expected = std::vector{flat::TagPair("C", "3")};
    info1 = (co_await MgmtdTestHelper(mgmtd).getNodeInfo(flat::NodeId(1)))->base();
    CO_ASSERT_EQ(setRes->info, info1);
  };
  folly::coro::blockingWait(f());
}
}  // namespace hf3fs::mgmtd::testing
