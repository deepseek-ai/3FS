#include "meta/service/MetaServer.h"

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>

#include "common/app/ApplicationBase.h"
#include "common/utils/Result.h"
#include "core/service/CoreService.h"
#include "fdb/HybridKvEngine.h"
#include "meta/components/ChainAllocator.h"
#include "meta/service/MetaOperator.h"
#include "meta/service/MetaSerdeService.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"

namespace hf3fs::meta::server {
MetaServer::MetaServer(const MetaServer::Config &config)
    : net::Server(config.base()),
      config_(config) {}

MetaServer::~MetaServer() { XLOGF(INFO, "Destructor MetaServer"); }

Result<Void> MetaServer::beforeStart() {
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

  // init service groups.
  if (!kvEngine_) {
    kvEngine_ = kv::HybridKvEngine::from(config_.kv_engine(), config_.use_memkv(), config_.fdb());
  }

  auto storageClient = storage::client::StorageClient::create(ClientId::random(appInfo().hostname),
                                                              config_.storage_client(),
                                                              *mgmtdClient_);
  XLOGF_IF(FATAL, !storageClient, "Failed to create storage client!");

  auto &appInfo = this->appInfo();
  XLOGF_IF(FATAL, !appInfo.nodeId, "Invalid nodeId {}", appInfo.nodeId);
  metaOperator_ = std::make_unique<MetaOperator>(
      config_.meta(),
      appInfo.nodeId,
      kvEngine_,
      mgmtdClient_,
      storageClient,
      std::make_unique<Forward>(config_.meta().forward(), appInfo.nodeId, *backgroundClient_, mgmtdClient_));
  RETURN_ON_ERROR(addSerdeService(std::make_unique<MetaSerdeService>(*metaOperator_), true));
  RETURN_ON_ERROR(addSerdeService(std::make_unique<core::CoreService>()));

  // init MetaOperator.
  std::optional<Layout> rootLayout;
  if (config_.use_memkv()) {
    rootLayout = Layout::newEmpty(ChainTableId(1), 512 << 10, 1);
  }
  auto result = folly::coro::blockingWait(metaOperator_->init(rootLayout));
  if (UNLIKELY(!result)) {
    XLOGF(ERR, "Init MetaOperator failed with {}", result.error().describe());
    RETURN_ON_ERROR(result);
  }

  metaOperator_->start(tpg().bgThreadPool());

  return Void{};
}

Result<Void> MetaServer::beforeStop() {
  metaOperator_->beforeStop();
  if (mgmtdClient_) {
    folly::coro::blockingWait(mgmtdClient_->stop());
  }
  return Void{};
}

Result<Void> MetaServer::afterStop() {
  metaOperator_->afterStop();
  if (backgroundClient_) {
    backgroundClient_->stopAndJoin();
  }
  return Void{};
}

Result<Void> MetaServer::start(const flat::AppInfo &info, std::shared_ptr<kv::IKVEngine> kvEngine) {
  kvEngine_ = std::move(kvEngine);
  return net::Server::start(info);
}

Result<Void> MetaServer::start(const flat::AppInfo &info,
                               std::unique_ptr<net::Client> client,
                               std::shared_ptr<::hf3fs::client::MgmtdClient> mgmtdClient) {
  backgroundClient_ = std::move(client);
  mgmtdClient_ = std::make_shared<::hf3fs::client::MgmtdClientForServer>(std::move(mgmtdClient));
  return net::Server::start(info);
}
}  // namespace hf3fs::meta::server
