#include "storage/service/StorageServer.h"

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/WithCancellation.h>
#include <folly/logging/xlog.h>

#include "common/kv/mem/MemKVEngine.h"
#include "common/utils/Result.h"
#include "core/service/CoreService.h"
#include "storage/service/ReliableForwarding.h"
#include "storage/service/StorageService.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"

namespace hf3fs::storage {

StorageServer::StorageServer(const Components::Config &config)
    : net::Server(config.base()),
      components_(config) {}

StorageServer::~StorageServer() {
  stopAndJoin();
  XLOGF(INFO, "Destructor StorageServer");
}

Result<Void> StorageServer::beforeStart() {
  RETURN_AND_LOG_ON_ERROR(addSerdeService(std::make_unique<StorageService>(components_.storageOperator), true));
  RETURN_AND_LOG_ON_ERROR(addSerdeService(std::make_unique<core::CoreService>()));
  groups().front()->setCoroutinesPoolGetter([this](const serde::MessagePacket<> &packet) -> DynamicCoroutinesPool & {
    switch (packet.serviceId) {
      case StorageSerde<>::kServiceID:
        return components_.getCoroutinesPool(packet.methodId);
      default:
        return components_.defaultPool;
    }
  });
  RETURN_AND_LOG_ON_ERROR(components_.start(appInfo(), tpg()));
  return Void{};
}

Result<Void> StorageServer::beforeStop() {
  components_.reliableUpdate.beforeStop();
  components_.reliableForwarding.beforeStop();
  return Void{};
}

Result<Void> StorageServer::afterStop() {
  RETURN_AND_LOG_ON_ERROR(components_.stopAndJoin(tpg().procThreadPool()));
  return Void{};
}

hf3fs::Result<Void> StorageServer::start(const flat::AppInfo &info,
                                         std::unique_ptr<::hf3fs::net::Client> client,
                                         std::shared_ptr<::hf3fs::client::MgmtdClient> mgmtdClient) {
  components_.netClient = std::move(client);
  components_.mgmtdClient = std::make_unique<hf3fs::client::MgmtdClientForServer>(std::move(mgmtdClient));
  return net::Server::start(info);
}

}  // namespace hf3fs::storage
