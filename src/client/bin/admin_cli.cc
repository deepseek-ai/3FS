#include <boost/algorithm/string.hpp>
#include <exception>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <iostream>

#include "client/cli/admin/AdminEnv.h"
#include "client/cli/admin/registerAdminCommands.h"
#include "client/cli/common/Utils.h"
#include "client/meta/MetaClient.h"
#include "client/mgmtd/MgmtdClientForAdmin.h"
#include "common/logging/LogInit.h"
#include "common/net/Client.h"
#include "common/net/ib/IBDevice.h"
#include "common/utils/ConfigBase.h"
#include "fdb/FDBContext.h"
#include "fdb/FDBKVEngine.h"
#include "stubs/MetaService/MetaServiceStub.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/core/CoreServiceStub.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"

DECLARE_bool(release_version);

using namespace hf3fs;
using namespace hf3fs::client;
using namespace hf3fs::client::cli;
using namespace hf3fs::stubs;
using hf3fs::client::CoreClient;
using hf3fs::meta::client::MetaClient;
using hf3fs::storage::client::StorageClient;

namespace {
class UserConfig : public ConfigBase<UserConfig> {
  CONFIG_ITEM(uid, int64_t{-1});
  CONFIG_ITEM(gid, int64_t{-1});
  CONFIG_ITEM(gids, std::vector<uint32_t>{});
  CONFIG_ITEM(token, "");
};

class Config : public ConfigBase<Config> {
  CONFIG_OBJ(client, net::Client::Config);
  CONFIG_OBJ(ib_devices, net::IBDevice::Config);
  CONFIG_ITEM(cluster_id, "");
  CONFIG_OBJ(user_info, UserConfig);
  CONFIG_ITEM(log, "DBG:normal; normal=file:path=cli.log,async=true,sync_level=ERR", ConfigCheckers::checkNotEmpty);
  CONFIG_ITEM(num_timeout_ms, 1000L);
  CONFIG_ITEM(verbose, false);
  CONFIG_ITEM(profile, false);
  CONFIG_ITEM(break_multi_line_command_on_failure, false);
  CONFIG_OBJ(fdb, kv::fdb::FDBConfig);
  CONFIG_OBJ(mgmtd_client, MgmtdClientForAdmin::Config, [&](auto &c) {
    c.set_enable_auto_refresh(true);
    c.set_auto_refresh_interval(1_s);
    c.set_enable_auto_heartbeat(false);
    c.set_enable_auto_extend_client_session(false);
  });
  CONFIG_OBJ(meta_client, MetaClient::Config);
  CONFIG_OBJ(storage_client, StorageClient::Config);
  CONFIG_OBJ(monitor, hf3fs::monitor::Monitor::Config);
};

flat::UserInfo generateUserInfo(const UserConfig &cfg) {
  auto uid = cfg.uid() != -1 ? cfg.uid() : static_cast<int64_t>(geteuid());
  auto gid = cfg.gid() != -1 ? cfg.gid() : static_cast<int64_t>(getegid());
  std::vector<flat::Gid> groups;
  if (cfg.gids().empty()) {
    auto n = getgroups(0, nullptr);
    std::vector<gid_t> gs(n);
    if (getgroups(n, gs.data()) < 0) {
      XLOGF(ERR, "failed to get supplementary groups for uid {}, going with egid {} only", uid, gid);
    }
    for (auto g : gs) groups.emplace_back(g);
  } else {
    for (auto g : cfg.gids()) groups.emplace_back(g);
  }
  return flat::UserInfo(flat::Uid(uid), flat::Gid(gid), std::move(groups), cfg.token());
}
}  // namespace

int main(int argc, char **argv) {
  Config config;
  auto initResult = config.init(&argc, &argv);
  XLOGF_IF(FATAL, !initResult, "Load config from flags failed with {}", initResult.error().describe());

  if (FLAGS_release_version) {
    fmt::print("{}\n{}\n", VersionInfo::full(), VersionInfo::commitHashFull());
    return 0;
  }

  logging::initOrDie(config.log());
  hf3fs::SysResource::increaseProcessFDLimit(524288);

  std::shared_ptr<net::Client> client;
  std::shared_ptr<kv::fdb::FDBContext> fdbContext;
  std::shared_ptr<kv::IKVEngine> kvEngine;
  std::shared_ptr<MgmtdClientForAdmin> mgmtdClient;
  std::shared_ptr<MetaClient> metaClient;
  std::shared_ptr<StorageClient> storageClient;
  std::shared_ptr<CoreClient> coreClient;
  auto clientId = ClientId::random();

  auto ensureIbInited = [&] {
    [[maybe_unused]] static bool inited = [&] {
      auto ibResult = hf3fs::net::IBManager::start(config.ib_devices());
      XLOGF_IF(FATAL, !ibResult, "Failed to start IBManager: {}.", ibResult.error().describe());
      return true;
    }();
  };
  SCOPE_EXIT { hf3fs::net::IBManager::stop(); };

  auto monitorResult = hf3fs::monitor::Monitor::start(config.monitor());
  XLOGF_IF(CRITICAL, !monitorResult, "Start monitor failed: {}", monitorResult.error().describe());
  SCOPE_EXIT { hf3fs::monitor::Monitor::stop(); };

  auto ensureClient = [&] {
    [[maybe_unused]] static bool inited = [&] {
      ensureIbInited();
      client = std::make_shared<net::Client>(config.client());
      client->start();
      return true;
    }();
  };

  auto ensureMgmtdClient = [&] {
    [[maybe_unused]] static bool inited = [&] {
      ensureClient();
      mgmtdClient =
          std::make_shared<MgmtdClientForAdmin>(config.cluster_id(),
                                                std::make_unique<RealStubFactory<mgmtd::MgmtdServiceStub>>(
                                                    [&client](net::Address addr) { return client->serdeCtx(addr); }),
                                                config.mgmtd_client());

      folly::coro::blockingWait(mgmtdClient->start(&client->tpg().bgThreadPool().randomPick()));
      folly::coro::blockingWait(mgmtdClient->refreshRoutingInfo(/*force=*/false));
      return true;
    }();
  };

  AdminEnv env;
  env.userInfo = generateUserInfo(config.user_info());
  env.kvEngineGetter = [&] {
    [[maybe_unused]] static bool inited = [&] {
      fdbContext = kv::fdb::FDBContext::create(config.fdb());
      kvEngine = std::make_unique<kv::FDBKVEngine>(fdbContext->getDB());
      return true;
    }();
    return kvEngine;
  };

  env.clientGetter = [&] {
    ensureClient();
    return client;
  };

  env.mgmtdClientGetter = [&] {
    ensureMgmtdClient();
    if (auto ri = mgmtdClient->getRoutingInfo(); !ri || !ri->raw())
      throw StatusException(Status(MgmtdClientCode::kRoutingInfoNotReady));
    return mgmtdClient;
  };

  env.unsafeMgmtdClientGetter = [&] {
    ensureMgmtdClient();
    return mgmtdClient;
  };

  env.storageClientGetter = [&] {
    [[maybe_unused]] static bool inited = [&] {
      ensureMgmtdClient();

      storageClient = StorageClient::create(clientId, config.storage_client(), *mgmtdClient);
      return true;
    }();
    if (auto ri = mgmtdClient->getRoutingInfo(); !ri || !ri->raw())
      throw StatusException(Status(MgmtdClientCode::kRoutingInfoNotReady));
    return storageClient;
  };

  env.metaClientGetter = [&] {
    [[maybe_unused]] static bool inited = [&] {
      ensureMgmtdClient();

      metaClient = std::make_shared<MetaClient>(
          clientId,
          config.meta_client(),
          std::make_unique<MetaClient::StubFactory>([&client](net::Address addr) { return client->serdeCtx(addr); }),
          mgmtdClient,
          env.storageClientGetter(),
          false);
      return true;
    }();
    if (auto ri = mgmtdClient->getRoutingInfo(); !ri || !ri->raw())
      throw StatusException(Status(MgmtdClientCode::kRoutingInfoNotReady));
    return metaClient;
  };

  env.coreClientGetter = [&] {
    [[maybe_unused]] static bool inited = [&] {
      ensureClient();
      coreClient = std::make_shared<CoreClient>(std::make_unique<RealStubFactory<core::CoreServiceStub>>(
          [&client](net::Address addr) { return client->serdeCtx(addr); }));
      return true;
    }();
    return coreClient;
  };

  std::string cmd = argc > 1 ? fmt::format("{}", fmt::join(&argv[1], &argv[argc], " ")) : "";
  XLOGF(INFO, "cli start cmd = \"{}\" verbose = {} profile = {}", cmd, config.verbose(), config.profile());

  Dispatcher dispatcher;

  auto result = folly::coro::blockingWait([&]() -> CoTryTask<void> {
    auto res = co_await registerAdminCommands(dispatcher);
    if (res.hasError()) {
      fmt::print("Register commands failed: {}", res.error());
      CO_RETURN_ERROR(res);
    }
    co_return co_await dispatcher.run(
        env,
        [&env] { return env.currentDir; },
        cmd,
        config.verbose(),
        config.profile(),
        config.break_multi_line_command_on_failure());
  }());

  if (mgmtdClient) {
    folly::coro::blockingWait(mgmtdClient->stop());
  }

  if (client) {
    client->stopAndJoin();
  }

  return result.hasError();
}
