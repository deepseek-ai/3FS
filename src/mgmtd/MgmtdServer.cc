#include "MgmtdServer.h"

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "common/app/ApplicationBase.h"
#include "core/app/ServerEnv.h"
#include "core/service/CoreService.h"
#include "fdb/HybridKvEngine.h"
#include "service/MgmtdService.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"

namespace hf3fs::mgmtd {

MgmtdServer::MgmtdServer(const Config &config)
    : net::Server(config.base()),
      config_(config) {}

MgmtdServer::~MgmtdServer() {}

Result<Void> MgmtdServer::beforeStart() {
  auto env = std::make_shared<core::ServerEnv>();
  env->setAppInfo(appInfo());
  XLOGF_IF(FATAL, !kvEngine_, "Should construct kv engine before server");
  env->setKvEngine(kvEngine_);
  env->setMgmtdStubFactory(std::make_shared<stubs::RealStubFactory<mgmtd::MgmtdServiceStub>>(serdeCtxCreator()));
  env->setBackgroundExecutor(&tpg().bgThreadPool());
  env->setConfigUpdater(ApplicationBase::updateConfig);
  env->setConfigValidater(ApplicationBase::validateConfig);

  mgmtdOperator_ = std::make_unique<MgmtdOperator>(std::move(env), config_.service());
  RETURN_ON_ERROR(addSerdeService(std::make_unique<MgmtdService>(*mgmtdOperator_)));
  RETURN_ON_ERROR(addSerdeService(std::make_unique<core::CoreService>()));

  mgmtdOperator_->start();
  return Void{};
}

Result<Void> MgmtdServer::afterStop() {
  mgmtdOperator_.reset();  // stop all background tasks
  // todo
  return Void{};
}

Result<Void> MgmtdServer::start(const flat::AppInfo &appInfo, std::shared_ptr<kv::IKVEngine> kvEngine) {
  kvEngine_ = std::move(kvEngine);
  return net::Server::start(appInfo);
}

}  // namespace hf3fs::mgmtd
