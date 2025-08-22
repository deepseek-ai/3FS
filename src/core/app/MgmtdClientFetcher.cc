#include "MgmtdClientFetcher.h"

#include <folly/experimental/coro/BlockingWait.h>

#include "common/app/ApplicationBase.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"

namespace hf3fs::core::launcher {
MgmtdClientFetcher::MgmtdClientFetcher(String clusterId,
                                       const net::Client::Config &clientCfg,
                                       const client::MgmtdClient::Config &mgmtdClientCfg)
    : clusterId_(std::move(clusterId)),
      clientCfg_(clientCfg),
      mgmtdClientCfg_(mgmtdClientCfg) {}

Result<flat::ConfigInfo> MgmtdClientFetcher::loadConfigTemplate(flat::NodeType nodeType) {
  RETURN_ON_ERROR(ensureClientInited());
  return folly::coro::blockingWait([&]() -> CoTryTask<flat::ConfigInfo> {
    XLOGF(INFO, "Start to load config from mgmtd");
    auto res = co_await mgmtdClient_->getConfig(nodeType, flat::ConfigVersion(0));
    XLOGF(INFO, "Load config from mgmtd finished. res: {}", res.hasError() ? res.error() : Status::OK);
    CO_RETURN_ON_ERROR(res);
    if (!*res) {
      XLOGF(INFO, "Load empty config from mgmtd");
      co_return flat::ConfigInfo::create();
    }
    XLOGF(INFO, "Load config from mgmtd version: {}", res->value().configVersion);
    co_return res->value();
  }());
}

void MgmtdClientFetcher::stopClient() {
  if (mgmtdClient_) {
    folly::coro::blockingWait(mgmtdClient_->stop());
    mgmtdClient_.reset();
    client_->stopAndJoin();
    client_.reset();
  }
}

Result<Void> MgmtdClientFetcher::ensureClientInited() {
  if (!client_) {
    auto client = std::make_unique<net::Client>(clientCfg_);
    RETURN_ON_ERROR(client->start("launcher"));
    client_ = std::move(client);
  }

  if (!mgmtdClient_) {
    auto ctxCreator =
        // client_ may be moved to Server later so here we should capture its raw pointer.
        [client = client_.get()](net::Address addr) { return client->serdeCtx(addr); };
    auto stub = std::make_unique<stubs::RealStubFactory<mgmtd::MgmtdServiceStub>>(std::move(ctxCreator));

    auto mgmtdClient = std::make_shared<client::MgmtdClient>(clusterId_, std::move(stub), mgmtdClientCfg_);

    folly::coro::blockingWait(
        mgmtdClient->start(&client_->tpg().bgThreadPool().randomPick(), /*startBackground=*/false));
    auto refreshRes = folly::coro::blockingWait(mgmtdClient->refreshRoutingInfo(/*force=*/false));
    RETURN_ON_ERROR(refreshRes);

    mgmtdClient_ = std::move(mgmtdClient);
  }
  return Void{};
}
}  // namespace hf3fs::core::launcher
