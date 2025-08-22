#include "simple_example/service/Server.h"

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>

#include "common/app/ApplicationBase.h"
#include "core/service/CoreService.h"
#include "simple_example/service/Service.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"

namespace hf3fs::simple_example::server {
SimpleExampleServer::SimpleExampleServer(const SimpleExampleServer::Config &config)
    : net::Server(config.base()),
      config_(config) {}

SimpleExampleServer::~SimpleExampleServer() { XLOGF(INFO, "Destroying SimpleExampleServer"); }

Result<Void> SimpleExampleServer::beforeStart() {
  if (!backgroundClient_) {
    backgroundClient_ = std::make_unique<net::Client>(config_.background_client());
    RETURN_ON_ERROR(backgroundClient_->start());
  }

  if (!mgmtdClient_) {
    auto ctxCreator = [this](net::Address addr) { return backgroundClient_->serdeCtx(addr); };
    mgmtdClient_ = std::make_shared<::hf3fs::client::MgmtdClientForServer>(
        appInfo().clusterId,
        std::make_unique<stubs::RealStubFactory<mgmtd::MgmtdServiceStub>>(std::move(ctxCreator)),
        config_.mgmtd_client());
  }

  mgmtdClient_->setAppInfoForHeartbeat(appInfo());
  mgmtdClient_->setConfigListener(ApplicationBase::updateConfig);
  mgmtdClient_->updateHeartbeatPayload(flat::MetaHeartbeatInfo{});
  folly::coro::blockingWait(mgmtdClient_->start(&tpg().bgThreadPool().randomPick()));
  auto mgmtdClientRefreshRes = folly::coro::blockingWait(mgmtdClient_->refreshRoutingInfo(/*force=*/false));
  XLOGF_IF(FATAL, !mgmtdClientRefreshRes, "Failed to refresh initial routing info!");

  auto storageClient = storage::client::StorageClient::create(ClientId::random(appInfo().hostname),
                                                              config_.storage_client(),
                                                              *mgmtdClient_);
  XLOGF_IF(FATAL, !storageClient, "Failed to create storage client!");

  auto appInfo = ApplicationBase::getAppInfo();
  XLOGF_IF(FATAL, !appInfo, "AppInfo not set!");
  XLOGF_IF(FATAL, !appInfo->nodeId, "Invalid nodeId {}", appInfo->nodeId);
  RETURN_ON_ERROR(addSerdeService(std::make_unique<SimpleExampleService>(), true));
  RETURN_ON_ERROR(addSerdeService(std::make_unique<core::CoreService>()));

  return Void{};
}

Result<Void> SimpleExampleServer::beforeStop() {
  folly::coro::blockingWait([this]() -> CoTask<void> {
    if (mgmtdClient_) {
      co_await mgmtdClient_->stop();
    }
  }());
  if (backgroundClient_) {
    backgroundClient_->stopAndJoin();
  }
  return Void{};
}

}  // namespace hf3fs::simple_example::server
