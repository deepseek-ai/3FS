#include <folly/experimental/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "client/meta/MetaClient.h"
#include "client/mgmtd/MgmtdClientForClient.h"
#include "common/logging/LogInit.h"
#include "common/net/Client.h"
#include "common/net/ib/IBDevice.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/SysResource.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"
#include "tools/commands/Commands.h"

DEFINE_bool(set_dir_layout, false, "Set default file layout for given directory");
DEFINE_bool(create_with_layout, false, "Create file/directory with given file layout");
DEFINE_bool(as_super, false, "Execute commands as super user");

using namespace hf3fs;
using namespace hf3fs::client;
using namespace hf3fs::flat;
using namespace hf3fs::stubs;
using namespace hf3fs::tools;
using hf3fs::meta::client::MetaClient;

class Config : public ConfigBase<Config> {
  CONFIG_OBJ(meta, MetaClient::Config);
  CONFIG_OBJ(mgmtd, MgmtdClientForClient::Config);

  CONFIG_OBJ(client, net::Client::Config);
  CONFIG_OBJ(ib_devices, net::IBDevice::Config);

  CONFIG_ITEM(cluster_id, "");
};

class TokenConfig : public ConfigBase<TokenConfig> {
  CONFIG_ITEM(token, "");
};

int main(int argc, char **argv) {
  google::SetCommandLineOptionWithMode("logging",
                                       "CRITICAL:out:err; out=stream:stream=stdout; err=stream:stream=stderr,level=ERR",
                                       google::SET_FLAGS_DEFAULT);

  auto home = std::string(getenv("HOME"));
  XLOGF(DBG, "home dir {}", home);
  auto cfg = home + "/.hf3fs/admin.toml";
  google::SetCommandLineOptionWithMode("cfg", cfg.c_str(), google::SET_FLAGS_DEFAULT);

  Config config;
  auto initResult = config.init(&argc, &argv);
  XLOGF_IF(FATAL, !initResult, "Load config from flags failed with {}", initResult.error().describe());

  auto ibResult = hf3fs::net::IBManager::start(config.ib_devices());
  XLOGF_IF(FATAL, !ibResult, "Failed to start IBManager: {}.", ibResult.error().describe());
  SCOPE_EXIT { hf3fs::net::IBManager::stop(); };

  net::Client client(config.client());
  client.start();

  auto makeCtx = [&client](net::Address addr) { return client.serdeCtx(addr); };
  // TODO: fill in real client id
  auto mgmtdClient =
      std::make_shared<MgmtdClientForClient>(config.cluster_id(),
                                             std::make_unique<RealStubFactory<mgmtd::MgmtdServiceStub>>(makeCtx),
                                             config.mgmtd());
  auto physicalHostnameRes = SysResource::hostname(/*physicalMachineName=*/true);
  XLOGF_IF(FATAL, !physicalHostnameRes, "Get physical hostname failed: {}", physicalHostnameRes.error());

  auto containerHostnameRes = SysResource::hostname(/*physicalMachineName=*/false);
  XLOGF_IF(FATAL, !containerHostnameRes, "Get container hostname failed: {}", containerHostnameRes.error());

  auto clientId = ClientId::random(*physicalHostnameRes);
  auto makeSerdeCtx = [&client](net::Address addr) { return client.serdeCtx(addr); };
  auto metaClient = std::make_unique<MetaClient>(clientId,
                                                 config.meta(),
                                                 std::make_unique<MetaClient::StubFactory>(makeSerdeCtx),
                                                 mgmtdClient,
                                                 nullptr,
                                                 false);

  folly::coro::blockingWait(mgmtdClient->start(&client.tpg().bgThreadPool().randomPick()));

  TokenConfig tokenConfig;
  tokenConfig.atomicallyUpdate(Path(home + "/.hf3fs/admin_token.toml"), false);

  UserInfo ui(Uid(FLAGS_as_super ? 0 : getuid()),
              Gid(FLAGS_as_super ? 0 : getgid()),
              {} /* groups */,
              tokenConfig.token());

  XLOGF(DBG, "uid {} gid {}", ui.uid, ui.gid);

  if (FLAGS_set_dir_layout) {
    setDirLayout(*metaClient, ui);
  } else if (FLAGS_create_with_layout) {
    createWithLayout(*metaClient, ui);
  }
}
