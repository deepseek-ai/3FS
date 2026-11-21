#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Sleep.h>

#include "client/mgmtd/MgmtdClient.h"
#include "fbs/mgmtd/NodeConversion.h"
#include "tests/GtestHelpers.h"
#include "tests/stubs/DummyMgmtdServiceStub.h"

namespace hf3fs::client::tests {
namespace {
class MgmtdClientTest : public ::testing::Test {
 protected:
  MgmtdClientTest() {}
};

#define CO_START_CLIENT(client) \
  co_await (client).start();    \
  co_await folly::coro::co_scope_exit([&]() -> CoTask<void> { co_await (client).stop(); })

std::vector<std::pair<String, String>> convert(const std::vector<std::pair<String, net::Address>> &v) {
  std::vector<std::pair<String, String>> ret;
  for (const auto &[method, addr] : v) {
    ret.emplace_back(method, addr.toString());
  }
  std::sort(ret.begin(), ret.end());
  return ret;
}

#define CO_ASSERT_RECORDS_EQ(expectedRecords)      \
  do {                                             \
    auto _expected = convert(expectedRecords);     \
    auto _actual = convert(Machine::visitRecords); \
    CO_ASSERT_EQ(_actual, _expected);              \
  } while (false)

flat::PersistentNodeInfo makePrimary(flat::NodeId id, net::Address addr) {
  flat::PersistentNodeInfo info;
  info.nodeId = id;
  flat::ServiceGroupInfo sgi;
  sgi.services.insert("Mgmtd");
  sgi.endpoints.push_back(addr);
  info.serviceGroups.push_back(sgi);
  return info;
}

struct Machine {
  Machine(flat::NodeId id, std::vector<net::Address> addrs) {
    selfInfo = makePrimary(id, addrs[0]);
    for (size_t i = 1; i < addrs.size(); ++i) {
      selfInfo.serviceGroups[0].endpoints.push_back(addrs[i]);
    }
  }

  std::unique_ptr<mgmtd::DummyMgmtdServiceStub> createStub(net::Address addr) {
    const auto &endpoints = selfInfo.serviceGroups[0].endpoints;
    auto it = std::find(endpoints.begin(), endpoints.end(), addr);
    if (it == endpoints.end()) return nullptr;

    auto stub = std::make_unique<mgmtd::DummyMgmtdServiceStub>();
    stub->set_getPrimaryMgmtdFunc([this, addr](const mgmtd::GetPrimaryMgmtdReq &) -> Result<mgmtd::GetPrimaryMgmtdRsp> {
          visitRecords.emplace_back("GetPrimaryMgmtd", addr);
          if (primary.hasError())
            return makeError(primary.error());
          else if (*primary)
            return mgmtd::GetPrimaryMgmtdRsp::create(**primary);
          return mgmtd::GetPrimaryMgmtdRsp::create(std::nullopt);
        })
        .set_getRoutingInfoFunc([this, addr](const mgmtd::GetRoutingInfoReq &) -> Result<mgmtd::GetRoutingInfoRsp> {
          visitRecords.emplace_back("GetRoutingInfo", addr);
          if (primary.hasError()) return makeError(primary.error());
          if (!*primary) return makeError(MgmtdCode::kNotPrimary);
          if (**primary != selfInfo) return makeError(MgmtdCode::kNotPrimary, fmt::format("{}", (*primary)->nodeId));
          if (routingInfo.hasError()) return makeError(routingInfo.error());
          if (*routingInfo) {
            return mgmtd::GetRoutingInfoRsp::create(**routingInfo);
          }
          return mgmtd::GetRoutingInfoRsp::create(std::nullopt);
        });
    return stub;
  }

  flat::PersistentNodeInfo selfInfo;
  Result<flat::PersistentNodeInfo *> primary = makeError(RPCCode::kConnectFailed);
  Result<flat::RoutingInfo *> routingInfo = makeError(RPCCode::kConnectFailed);
  static std::vector<std::pair<String, net::Address>> visitRecords;
};

std::vector<std::pair<String, net::Address>> Machine::visitRecords;

TEST_F(MgmtdClientTest, testStartStopWithWrongConfig) {
  MgmtdClient::Config config;
  config.set_auto_refresh_interval(1_ms);
  config.set_auto_heartbeat_interval(1_ms);
  MgmtdClient client("", nullptr, config);
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_START_CLIENT(client);
    co_await folly::coro::sleep(std::chrono::milliseconds(100));
  }());
}

TEST_F(MgmtdClientTest, testRepetitiveStartStop) {
  MgmtdClient::Config config;
  config.set_auto_refresh_interval(1_ms);
  config.set_auto_heartbeat_interval(1_ms);
  MgmtdClient client("", nullptr, config);
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_START_CLIENT(client);
    co_await folly::coro::sleep(std::chrono::milliseconds(10));
    co_await client.stop();

    co_await client.start();
    co_await folly::coro::sleep(std::chrono::milliseconds(10));
    co_await client.stop();

    co_await client.start();
    co_await folly::coro::sleep(std::chrono::milliseconds(10));
    co_await client.stop();
  }());
}

TEST_F(MgmtdClientTest, testGetRoutingInfoBeforeStart) {
  MgmtdClient::Config config;
  MgmtdClient client("", nullptr, config);
  auto info = client.getRoutingInfo();
  ASSERT_TRUE(!info);
}

net::Address ada = net::Address::from("127.0.0.1:8080").value();
net::Address adb = net::Address::from("192.168.0.1:9000").value();
net::Address adc = net::Address::from("192.168.0.2:8000").value();
net::Address add = net::Address::from("192.168.0.3:8800").value();
net::Address ade;  // invalid addr
net::Address adf = net::Address::from("RDMA://127.0.0.1:8080").value();
net::Address adg = net::Address::from("RDMA://192.168.0.1:9000").value();
net::Address adh = net::Address::from("RDMA://192.168.0.2:8000").value();
net::Address adi = net::Address::from("RDMA://192.168.0.3:8800").value();
net::Address adj = net::Address::from("192.168.0.4:8888").value();

TEST_F(MgmtdClientTest, testWhenNoPrimary) {
  struct StubFactory : public stubs::IStubFactory<mgmtd::IMgmtdServiceStub> {
    std::unique_ptr<mgmtd::IMgmtdServiceStub> create(net::Address) {
      return std::make_unique<mgmtd::DummyMgmtdServiceStub>();
    }
  };

  MgmtdClient::Config config;
  config.set_mgmtd_server_addresses({to_named(ada), to_named(adb), to_named(adc)});
  config.set_enable_auto_refresh(false);
  config.set_enable_auto_heartbeat(false);
  MgmtdClient client("", std::make_unique<StubFactory>(), config);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_START_CLIENT(client);
    CO_ASSERT_ERROR(co_await client.refreshRoutingInfo(false), MgmtdClientCode::kPrimaryMgmtdNotFound);
    CO_ASSERT_TRUE(!client.getRoutingInfo());
  }());
}

TEST_F(MgmtdClientTest, testInvalidAddress) {
  MgmtdClient::Config config;
  config.set_mgmtd_server_addresses({to_named(ada), to_named(ade)});
  MgmtdClient client("", nullptr, config);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_START_CLIENT(client);
    CO_ASSERT_ERROR(co_await client.refreshRoutingInfo(false), StatusCode::kInvalidConfig);
  }());
}

TEST_F(MgmtdClientTest, testAddressTypeMismatch) {
  MgmtdClient::Config config;
  config.set_mgmtd_server_addresses({to_named(ada), to_named(adf)});
  config.set_network_type(net::Address::TCP);
  MgmtdClient client("", nullptr, config);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_START_CLIENT(client);
    CO_ASSERT_ERROR(co_await client.refreshRoutingInfo(false), StatusCode::kInvalidConfig);
  }());
}

TEST_F(MgmtdClientTest, testWhenLastIsPrimary) {
  struct StubFactory : public stubs::IStubFactory<mgmtd::IMgmtdServiceStub> {
    std::unique_ptr<mgmtd::IMgmtdServiceStub> create(net::Address addr) {
      auto stub = std::make_unique<mgmtd::DummyMgmtdServiceStub>();
      if (addr == adc) {
        stub->set_getPrimaryMgmtdFunc([](const mgmtd::GetPrimaryMgmtdReq &) {
              return mgmtd::GetPrimaryMgmtdRsp::create(makePrimary(flat::NodeId(1), adc));
            })
            .set_getRoutingInfoFunc(
                [](const mgmtd::GetRoutingInfoReq &) { return mgmtd::GetRoutingInfoRsp::create(std::nullopt); });
      }
      return stub;
    }
  };

  MgmtdClient::Config config;
  config.set_mgmtd_server_addresses({to_named(ada), to_named(adb), to_named(adc)});
  config.set_enable_auto_refresh(false);
  config.set_enable_auto_heartbeat(false);
  MgmtdClient client("", std::make_unique<StubFactory>(), config);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_START_CLIENT(client);
    CO_ASSERT_OK(co_await client.refreshRoutingInfo(false));
    auto ri = client.getRoutingInfo();
    CO_ASSERT_TRUE(ri);
    CO_ASSERT_TRUE(!ri->raw());
  }());
}

TEST_F(MgmtdClientTest, testWhenPrimaryNotInConfig) {
  struct StubFactory : public stubs::IStubFactory<mgmtd::IMgmtdServiceStub> {
    std::unique_ptr<mgmtd::IMgmtdServiceStub> create(net::Address addr) {
      auto stub = std::make_unique<mgmtd::DummyMgmtdServiceStub>();
      if (addr == adc) {
        stub->set_getPrimaryMgmtdFunc([](const mgmtd::GetPrimaryMgmtdReq &) {
          return mgmtd::GetPrimaryMgmtdRsp::create(makePrimary(flat::NodeId(1), add));
        });
      } else if (addr == add) {
        stub->set_getPrimaryMgmtdFunc([](const mgmtd::GetPrimaryMgmtdReq &) {
              return mgmtd::GetPrimaryMgmtdRsp::create(makePrimary(flat::NodeId(1), add));
            })
            .set_getRoutingInfoFunc(
                [](const mgmtd::GetRoutingInfoReq &) { return mgmtd::GetRoutingInfoRsp::create(std::nullopt); });
      }
      return stub;
    }
  };

  MgmtdClient::Config config;
  config.set_mgmtd_server_addresses({to_named(ada), to_named(adb), to_named(adc)});
  config.set_enable_auto_refresh(false);
  config.set_enable_auto_heartbeat(false);
  MgmtdClient client("", std::make_unique<StubFactory>(), config);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_START_CLIENT(client);
    CO_ASSERT_OK(co_await client.refreshRoutingInfo(false));
    auto ri = client.getRoutingInfo();
    CO_ASSERT_TRUE(ri);
    CO_ASSERT_TRUE(!ri->raw());
  }());
}

TEST_F(MgmtdClientTest, testWhenGetPrimaryLoop) {
  std::vector<net::Address> addresses = {ada, adb, adc};
  struct StubFactory : public stubs::IStubFactory<mgmtd::IMgmtdServiceStub> {
    std::vector<net::Address> addresses;

    explicit StubFactory(std::vector<net::Address> v)
        : addresses(std::move(v)) {}

    std::unique_ptr<mgmtd::IMgmtdServiceStub> create(net::Address addr) {
      auto stub = std::make_unique<mgmtd::DummyMgmtdServiceStub>();
      for (size_t i = 0; i < addresses.size(); ++i) {
        if (addr == addresses[i]) {
          if (i + 1 == addresses.size()) {
            stub->set_getPrimaryMgmtdFunc([this](const mgmtd::GetPrimaryMgmtdReq &) {
              return mgmtd::GetPrimaryMgmtdRsp::create(makePrimary(flat::NodeId(1), addresses[0]));
            });
          } else {
            stub->set_getPrimaryMgmtdFunc([this, i](const mgmtd::GetPrimaryMgmtdReq &) {
              return mgmtd::GetPrimaryMgmtdRsp::create(makePrimary(flat::NodeId(i + 2), addresses[i + 1]));
            });
          }
          break;
        }
      }
      return stub;
    }
  };

  MgmtdClient::Config config;
  config.set_mgmtd_server_addresses(to_named(addresses));
  config.set_enable_auto_refresh(false);
  config.set_enable_auto_heartbeat(false);
  MgmtdClient client("", std::make_unique<StubFactory>(addresses), config);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_START_CLIENT(client);
    CO_ASSERT_ERROR(co_await client.refreshRoutingInfo(false), MgmtdClientCode::kPrimaryMgmtdNotFound);
    CO_ASSERT_TRUE(!client.getRoutingInfo());
  }());
}

TEST_F(MgmtdClientTest, testRetryOnRefreshFail) {
  std::vector<net::Address> addresses = {ada, adb, adc};
  struct StubFactory : public stubs::IStubFactory<mgmtd::IMgmtdServiceStub> {
    int counta = 0;
    int countb = 0;

    std::unique_ptr<mgmtd::IMgmtdServiceStub> create(net::Address addr) {
      auto stub = std::make_unique<mgmtd::DummyMgmtdServiceStub>();
      if (addr == ada) {
        stub->set_getPrimaryMgmtdFunc([this](const mgmtd::GetPrimaryMgmtdReq &) -> Result<mgmtd::GetPrimaryMgmtdRsp> {
              if (++counta == 1) {
                return mgmtd::GetPrimaryMgmtdRsp::create(makePrimary(flat::NodeId(1), ada));
              } else {
                return makeError(RPCCode::kConnectFailed);
              }
            })
            .set_getRoutingInfoFunc([this](const mgmtd::GetRoutingInfoReq &) -> Result<mgmtd::GetRoutingInfoRsp> {
              if (++countb == 1)
                return mgmtd::GetRoutingInfoRsp::create(std::nullopt);
              else
                return makeError(RPCCode::kConnectFailed);
            });
      } else if (addr == adb) {
        stub->set_getPrimaryMgmtdFunc([](const mgmtd::GetPrimaryMgmtdReq &) {
              return mgmtd::GetPrimaryMgmtdRsp::create(makePrimary(flat::NodeId(2), adb));
            })
            .set_getRoutingInfoFunc([](const mgmtd::GetRoutingInfoReq &) {
              flat::RoutingInfo ri;
              ri.routingInfoVersion = flat::RoutingInfoVersion(2);
              return mgmtd::GetRoutingInfoRsp::create(ri);
            });
      }
      return stub;
    }
  };

  MgmtdClient::Config config;
  config.set_mgmtd_server_addresses(to_named(addresses));
  config.set_enable_auto_refresh(false);
  config.set_enable_auto_heartbeat(false);
  MgmtdClient client("", std::make_unique<StubFactory>(), config);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_START_CLIENT(client);
    CO_ASSERT_OK(co_await client.refreshRoutingInfo(false));
    auto ri = client.getRoutingInfo();
    CO_ASSERT_TRUE(ri && !ri->raw());
    CO_ASSERT_OK(co_await client.refreshRoutingInfo(false));
    ri = client.getRoutingInfo();
    CO_ASSERT_TRUE(ri && ri->raw());
    CO_ASSERT_EQ(ri->raw()->routingInfoVersion, flat::RoutingInfoVersion(2));
  }());
}

TEST_F(MgmtdClientTest, testRefreshRoutingInfoCallback) {
  std::vector<net::Address> addresses = {ada};
  struct StubFactory : public stubs::IStubFactory<mgmtd::IMgmtdServiceStub> {
    std::unique_ptr<mgmtd::IMgmtdServiceStub> create(net::Address) {
      auto stub = std::make_unique<mgmtd::DummyMgmtdServiceStub>();
      stub->set_getPrimaryMgmtdFunc([](const mgmtd::GetPrimaryMgmtdReq &) {
            return mgmtd::GetPrimaryMgmtdRsp::create(makePrimary(flat::NodeId(1), ada));
          })
          .set_getRoutingInfoFunc([](const mgmtd::GetRoutingInfoReq &req) {
            flat::RoutingInfo ri;
            ri.routingInfoVersion = flat::RoutingInfoVersion(req.routingInfoVersion + 1);
            return mgmtd::GetRoutingInfoRsp::create(ri);
          });
      return stub;
    }
  };

  MgmtdClient::Config config;
  config.set_mgmtd_server_addresses(to_named(addresses));
  config.set_enable_auto_refresh(false);
  config.set_enable_auto_heartbeat(false);
  MgmtdClient client("", std::make_unique<StubFactory>(), config);

  std::vector<std::shared_ptr<RoutingInfo>> ris;
  client.addRoutingInfoListener("test", [&](std::shared_ptr<RoutingInfo> ri) { ris.push_back(std::move(ri)); });

  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_START_CLIENT(client);
    for (int i = 0; i < 10; ++i) CO_ASSERT_OK(co_await client.refreshRoutingInfo(false));
  }());

  ASSERT_EQ(ris.size(), 10);
  for (int i = 0; i < 10; ++i) {
    const auto &ri = ris[i];
    ASSERT_TRUE(ri && ri->raw());
    ASSERT_EQ(ri->raw()->routingInfoVersion, flat::RoutingInfoVersion(i + 1));
    if (i > 0) ASSERT_TRUE(ri->lastRefreshTime() > ris[i - 1]->lastRefreshTime());
  }
}

TEST_F(MgmtdClientTest, testRetryAllAvailableAddresses) {
  Machine::visitRecords.clear();
  Machine ma(flat::NodeId(1), {ada, adb, adc});
  Machine mb(flat::NodeId(2), {add, adf});
  Machine mc(flat::NodeId(3), {adj});

  struct StubFactory : public stubs::IStubFactory<mgmtd::IMgmtdServiceStub> {
    std::vector<Machine *> machines;

    explicit StubFactory(std::vector<Machine *> v)
        : machines(std::move(v)) {}

    std::unique_ptr<mgmtd::IMgmtdServiceStub> create(net::Address addr) {
      for (auto *m : machines) {
        auto stub = m->createStub(addr);
        if (stub) return stub;
      }
      return nullptr;
    }
  };

  MgmtdClient::Config config;
  config.set_mgmtd_server_addresses({to_named(adj)});
  config.set_enable_auto_refresh(false);
  config.set_enable_auto_heartbeat(false);
  config.set_network_type(net::Address::TCP);
  MgmtdClient client("", std::make_unique<StubFactory>(std::vector{&ma, &mb, &mc}), config);

  flat::RoutingInfo ri;
  ri.routingInfoVersion = flat::RoutingInfoVersion(2);
  ri.nodes[ma.selfInfo.nodeId] = flat::toNode(ma.selfInfo);
  ri.nodes[mb.selfInfo.nodeId] = flat::toNode(mb.selfInfo);
  ri.nodes[mc.selfInfo.nodeId] = flat::toNode(mc.selfInfo);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    mc.primary = &mc.selfInfo;
    mc.routingInfo = &ri;
    CO_START_CLIENT(client);

    Machine::visitRecords.clear();
    CO_ASSERT_OK(co_await client.refreshRoutingInfo(false));
    {
      std::vector<std::pair<String, net::Address>> expected = {{"GetPrimaryMgmtd", adj}, {"GetRoutingInfo", adj}};
      CO_ASSERT_RECORDS_EQ(expected);
    }

    mc.primary = makeError(RPCCode::kConnectFailed);
    Machine::visitRecords.clear();
    CO_ASSERT_ERROR(co_await client.refreshRoutingInfo(false), MgmtdClientCode::kPrimaryMgmtdNotFound);
    {
      std::vector<std::pair<String, net::Address>> expected = {
          {"GetRoutingInfo", adj},
          {"GetPrimaryMgmtd", ada},
          {"GetPrimaryMgmtd", adb},
          {"GetPrimaryMgmtd", adc},
          {"GetPrimaryMgmtd", add},
      };
      CO_ASSERT_RECORDS_EQ(expected);
    }
  }());
}

TEST_F(MgmtdClientTest, testRetryEndWhenNoPrimary) {
  Machine::visitRecords.clear();
  Machine ma(flat::NodeId(1), {ada, adb, adc});
  Machine mb(flat::NodeId(2), {add, adf});
  Machine mc(flat::NodeId(3), {adj});

  struct StubFactory : public stubs::IStubFactory<mgmtd::IMgmtdServiceStub> {
    std::vector<Machine *> machines;

    explicit StubFactory(std::vector<Machine *> v)
        : machines(std::move(v)) {}

    std::unique_ptr<mgmtd::IMgmtdServiceStub> create(net::Address addr) {
      for (auto *m : machines) {
        auto stub = m->createStub(addr);
        if (stub) return stub;
      }
      return nullptr;
    }
  };

  MgmtdClient::Config config;
  config.set_mgmtd_server_addresses({to_named(adj)});
  config.set_enable_auto_refresh(false);
  config.set_enable_auto_heartbeat(false);
  config.set_network_type(net::Address::TCP);
  MgmtdClient client("", std::make_unique<StubFactory>(std::vector{&ma, &mb, &mc}), config);

  flat::RoutingInfo ri;
  ri.routingInfoVersion = flat::RoutingInfoVersion(2);
  ri.nodes[ma.selfInfo.nodeId] = flat::toNode(ma.selfInfo);
  ri.nodes[mb.selfInfo.nodeId] = flat::toNode(mb.selfInfo);
  ri.nodes[mc.selfInfo.nodeId] = flat::toNode(mc.selfInfo);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    mc.primary = &mc.selfInfo;
    mc.routingInfo = &ri;
    CO_START_CLIENT(client);

    Machine::visitRecords.clear();
    CO_ASSERT_OK(co_await client.refreshRoutingInfo(false));
    {
      std::vector<std::pair<String, net::Address>> expected = {{"GetPrimaryMgmtd", adj}, {"GetRoutingInfo", adj}};
      CO_ASSERT_RECORDS_EQ(expected);
    }

    ma.primary = nullptr;
    mc.primary = makeError(RPCCode::kConnectFailed);
    Machine::visitRecords.clear();
    CO_ASSERT_ERROR(co_await client.refreshRoutingInfo(false), MgmtdClientCode::kPrimaryMgmtdNotFound);
    {
      std::vector<std::pair<String, net::Address>> expected = {
          {"GetRoutingInfo", adj},
          {"GetPrimaryMgmtd", ada},
      };
      CO_ASSERT_RECORDS_EQ(expected);
    }
  }());
}

TEST_F(MgmtdClientTest, testRetryUnknownAddrs) {
  Machine::visitRecords.clear();
  Machine ma(flat::NodeId(1), {ada, adb, adc});
  Machine mb(flat::NodeId(2), {add, adf});
  Machine mc(flat::NodeId(3), {adj});

  struct StubFactory : public stubs::IStubFactory<mgmtd::IMgmtdServiceStub> {
    std::vector<Machine *> machines;

    explicit StubFactory(std::vector<Machine *> v)
        : machines(std::move(v)) {}

    std::unique_ptr<mgmtd::IMgmtdServiceStub> create(net::Address addr) {
      for (auto *m : machines) {
        auto stub = m->createStub(addr);
        if (stub) return stub;
      }
      return nullptr;
    }
  };

  MgmtdClient::Config config;
  config.set_mgmtd_server_addresses({to_named(adj), to_named(add)});
  config.set_enable_auto_refresh(false);
  config.set_enable_auto_heartbeat(false);
  config.set_network_type(net::Address::TCP);
  MgmtdClient client("", std::make_unique<StubFactory>(std::vector{&ma, &mb, &mc}), config);

  flat::RoutingInfo ri;
  ri.routingInfoVersion = flat::RoutingInfoVersion(2);
  ri.nodes[ma.selfInfo.nodeId] = flat::toNode(ma.selfInfo);
  ri.nodes[mc.selfInfo.nodeId] = flat::toNode(mc.selfInfo);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    mc.primary = &mc.selfInfo;
    mc.routingInfo = &ri;
    CO_START_CLIENT(client);

    Machine::visitRecords.clear();
    CO_ASSERT_OK(co_await client.refreshRoutingInfo(false));
    {
      std::vector<std::pair<String, net::Address>> expected = {{"GetPrimaryMgmtd", adj}, {"GetRoutingInfo", adj}};
      CO_ASSERT_RECORDS_EQ(expected);
    }

    mc.primary = makeError(RPCCode::kConnectFailed);
    Machine::visitRecords.clear();
    CO_ASSERT_ERROR(co_await client.refreshRoutingInfo(false), MgmtdClientCode::kPrimaryMgmtdNotFound);
    {
      std::vector<std::pair<String, net::Address>> expected = {
          {"GetRoutingInfo", adj},
          {"GetPrimaryMgmtd", ada},
          {"GetPrimaryMgmtd", adb},
          {"GetPrimaryMgmtd", adc},
          {"GetPrimaryMgmtd", add},
      };
      CO_ASSERT_RECORDS_EQ(expected);
    }
  }());
}

TEST_F(MgmtdClientTest, testSetGetConfigViaInvoke) {
  struct StubFactory : public stubs::IStubFactory<mgmtd::IMgmtdServiceStub> {
    std::map<flat::NodeType, flat::ConfigInfo> configs;
    std::unique_ptr<mgmtd::IMgmtdServiceStub> create(net::Address) {
      auto stub = std::make_unique<mgmtd::DummyMgmtdServiceStub>();
      stub->set_setConfigFunc([this](const mgmtd::SetConfigReq &req) -> Result<mgmtd::SetConfigRsp> {
            auto &info = configs[req.nodeType];
            auto cv = flat::ConfigVersion(info.configVersion + 1);
            info = flat::ConfigInfo::create(cv, req.content, req.desc);
            return mgmtd::SetConfigRsp::create(cv);
          })
          .set_getConfigFunc([this](const mgmtd::GetConfigReq &req) -> Result<mgmtd::GetConfigRsp> {
            if (configs.contains(req.nodeType)) return mgmtd::GetConfigRsp::create(configs[req.nodeType]);
            return mgmtd::GetConfigRsp::create(std::nullopt);
          })
          .set_getPrimaryMgmtdFunc([](const mgmtd::GetPrimaryMgmtdReq &) {
            return mgmtd::GetPrimaryMgmtdRsp::create(makePrimary(flat::NodeId(1), ada));
          });
      return stub;
    }
  };

  MgmtdClient::Config config;
  config.set_mgmtd_server_addresses({to_named(ada)});
  config.set_enable_auto_refresh(false);
  config.set_enable_auto_heartbeat(false);
  MgmtdClient client("", std::make_unique<StubFactory>(), config);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_START_CLIENT(client);
    {
      auto res = co_await client.setConfig(flat::UserInfo{}, flat::NodeType::MGMTD, String("abcd"), "desc");
      CO_ASSERT_OK(res);
    }
    {
      auto res = co_await client.getConfig(flat::NodeType::MGMTD, flat::ConfigVersion{0});
      CO_ASSERT_OK(res);
      CO_ASSERT_TRUE(res->has_value());
      CO_ASSERT_EQ(res->value().configVersion, flat::ConfigVersion{1});
      CO_ASSERT_EQ(res->value().content, "abcd");
      CO_ASSERT_EQ(res->value().desc, "desc");

      res = co_await client.getConfig(flat::NodeType::META, flat::ConfigVersion{0});
      CO_ASSERT_OK(res);
      CO_ASSERT_TRUE(!res->has_value());
    }
  }());
}

}  // namespace
}  // namespace hf3fs::client::tests
