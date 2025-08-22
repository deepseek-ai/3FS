#include <chrono>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Sleep.h>

#include "common/kv/mem/MemKVEngine.h"
#include "meta/service/MetaServer.h"
#include "mgmtd/MgmtdServer.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"
#include "tests/GtestHelpers.h"
#include "tests/client/ClientWithConfig.h"
#include "tests/client/ServerWithConfig.h"

namespace hf3fs::client::tests {
namespace {
class MgmtdClusterTest : public ::testing::Test {
 protected:
  MgmtdClusterTest() = default;
};

#define CO_START_CLIENT(client) \
  co_await (client).start();    \
  co_await folly::coro::co_scope_exit([&]() -> CoTask<void> { co_await (client).stop(); })

struct ClientWithConfig {
  net::Client::Config config;
  std::unique_ptr<net::Client> client;

  Result<Void> init() {
    client = std::make_unique<net::Client>(config);
    return client->start("test");
  }
};

TEST_F(MgmtdClusterTest, testMgmtdServerRestart) {
  const String clusterId = "cluster";
  auto kvEngine = std::make_shared<kv::MemKVEngine>();

  ClientWithConfig client;
  ASSERT_OK(client.init());

  MgmtdServerWithConfig mgmtdServer(clusterId, flat::NodeId(1));
  ASSERT_OK(mgmtdServer.start(kvEngine));

  MgmtdClientWithConfig mgmtdClient(
      clusterId,
      [&client](net::Address addr) { return client.client->serdeCtx(addr); },
      mgmtdServer.collectAddressList("Mgmtd"));

  folly::coro::blockingWait([&]() -> CoTask<void> {
    co_await folly::coro::sleep(std::chrono::milliseconds(500));

    CO_START_CLIENT(*mgmtdClient.client);
    // set chain table
    flat::ChainTableId tableId{1};
    std::vector<flat::ChainSetting> chains;
    std::vector<flat::ChainId> chainIds;
    {
      for (int i = 0, t = 0; i < 10; ++i) {
        flat::ChainSetting chain;
        chain.chainId = flat::ChainId((tableId << 16) + i + 1);
        for (int j = 0; j < 3; ++j) {
          flat::ChainTargetSetting target;
          target.targetId = flat::TargetId(++t);
          chain.targets.push_back(target);
        }
        chainIds.push_back(chain.chainId);
        chains.push_back(std::move(chain));
      }

      CO_ASSERT_OK(co_await mgmtdClient.rawClient->setChains(flat::UserInfo{}, chains));
      CO_ASSERT_OK(co_await mgmtdClient.rawClient->setChainTable(flat::UserInfo{}, tableId, chainIds, ""));
      CO_AWAIT_ASSERT_OK(mgmtdClient->refreshRoutingInfo(false));

      auto ri = mgmtdClient->getRoutingInfo();
      CO_ASSERT_TRUE(ri && ri->raw());

      for (const auto &c : chains) {
        auto ci = ri->getChain(c.chainId);
        CO_ASSERT_TRUE(ci);
        CO_ASSERT_EQ(ci->chainId, c.chainId);
      }
    }
    // restart mgmtd, expect chain table exists
    {
      CO_ASSERT_OK(mgmtdServer.restart(kvEngine));
      auto ri = mgmtdClient->getRoutingInfo();
      CO_ASSERT_TRUE(ri && ri->raw());

      for (const auto &c : chains) {
        auto ci = ri->getChain(c.chainId);
        CO_ASSERT_TRUE(ci);
        CO_ASSERT_EQ(ci->chainId, c.chainId);
      }
    }
  }());
}
}  // namespace
}  // namespace hf3fs::client::tests
