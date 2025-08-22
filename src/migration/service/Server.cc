#include "migration/service/Server.h"

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>

#include "common/app/ApplicationBase.h"
#include "core/service/CoreService.h"
#include "migration/service/Service.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"

namespace hf3fs::migration::server {
MigrationServer::MigrationServer(const MigrationServer::Config &config)
    : net::Server(config.base()),
      config_(config),
      clientId_(ClientId::random()) {}

MigrationServer::~MigrationServer() { XLOGF(INFO, "Destroying MigrationServer"); }

Result<Void> MigrationServer::beforeStart() {
  if (!backgroundClient_) {
    backgroundClient_ = std::make_unique<net::Client>(config_.background_client());
    RETURN_ON_ERROR(backgroundClient_->start());
  }

  auto appInfo = this->appInfo();
  RETURN_ON_ERROR(addSerdeService(std::make_unique<MigrationService>(), true));
  RETURN_ON_ERROR(addSerdeService(std::make_unique<core::CoreService>()));

  auto stubFactory = std::make_unique<hf3fs::stubs::RealStubFactory<hf3fs::mgmtd::MgmtdServiceStub>>(
      hf3fs::stubs::ClientContextCreator{[this](net::Address addr) { return backgroundClient_->serdeCtx(addr); }});
  auto mgmtdClient = std::make_unique<hf3fs::client::MgmtdClientForClient>(appInfo.clusterId,
                                                                           std::move(stubFactory),
                                                                           config_.mgmtd_client());

  auto physicalHostnameRes = SysResource::hostname(/*physicalMachineName=*/true);
  if (!physicalHostnameRes) {
    XLOGF(ERR, "getHostname(true) failed: {}", physicalHostnameRes.error());
    return makeError(StatusCode::kInvalidConfig);
  }

  auto containerHostnameRes = SysResource::hostname(/*physicalMachineName=*/false);
  if (!containerHostnameRes) {
    XLOGF(ERR, "getHostname(false) failed: {}", containerHostnameRes.error());
    return makeError(StatusCode::kInvalidConfig);
  }

  mgmtdClient->setClientSessionPayload({clientId_.uuid.toHexString(),
                                        flat::NodeType::CLIENT,
                                        flat::ClientSessionData::create(
                                            /*universalId=*/*physicalHostnameRes,
                                            /*description=*/fmt::format("Migration: {}", *containerHostnameRes),
                                            /*serviceGroups=*/std::vector<flat::ServiceGroupInfo>{},
                                            flat::ReleaseVersion::fromVersionInfo()),
                                        flat::UserInfo{}});
  folly::coro::blockingWait(mgmtdClient->start(&backgroundClient_->tpg().bgThreadPool().randomPick()));
  mgmtdClient_ = std::move(mgmtdClient);

  auto storageClient = storage::client::StorageClient::create(clientId_, config_.storage_client(), *mgmtdClient_);
  if (!storageClient) {
    XLOGF(ERR, "Failed to create storage client!");
    return makeError(StatusCode::kInvalidConfig);
  }

  return Void{};
}

Result<Void> MigrationServer::beforeStop() {
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

}  // namespace hf3fs::migration::server
