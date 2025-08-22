#include "MgmtdConfigFetcher.h"

#include <folly/experimental/coro/BlockingWait.h>

#include "MgmtdServer.h"
#include "common/app/ApplicationBase.h"
#include "common/kv/WithTransaction.h"
#include "fdb/FDBRetryStrategy.h"
#include "fdb/HybridKvEngine.h"
#include "mgmtd/store/MgmtdStore.h"

namespace hf3fs::mgmtd {
namespace {
kv::FDBRetryStrategy getRetryStrategy() { return kv::FDBRetryStrategy({1_s, 10, false}); }
}  // namespace

void MgmtdConfigFetcher::init(const kv::HybridKvEngineConfig &config,
                              bool useMemKV,
                              const kv::fdb::FDBConfig &fdbConfig) {
  kvEngine_ = kv::HybridKvEngine::from(config, useMemKV, fdbConfig);
}

Result<flat::ConfigInfo> MgmtdConfigFetcher::loadConfigTemplate(flat::NodeType nodeType) {
  XLOGF_IF(FATAL, nodeType != flat::NodeType::MGMTD, "Unexpected NodeType: {}", static_cast<int>(nodeType));
  MgmtdStore store;
  auto handler = [&](kv::IReadOnlyTransaction &txn) -> CoTryTask<flat::ConfigInfo> {
    auto res = co_await store.loadLatestConfig(txn, flat::NodeType::MGMTD);
    CO_RETURN_ON_ERROR(res);
    if (res->has_value()) {
      co_return std::move(res->value());
    }
    co_return flat::ConfigInfo::create();
  };
  return folly::coro::blockingWait(
      kv::WithTransaction(getRetryStrategy()).run(kvEngine_->createReadonlyTransaction(), std::move(handler)));
}

Result<Void> MgmtdConfigFetcher::completeAppInfo(flat::AppInfo &appInfo) {
  MgmtdStore store;
  auto handler = [&](kv::IReadOnlyTransaction &txn) -> CoTryTask<void> {
    auto res = co_await store.loadNodeInfo(txn, appInfo.nodeId);
    CO_RETURN_ON_ERROR(res);
    if (res->has_value()) {
      appInfo.tags = res->value().tags;
    }
    co_return Void{};
  };
  return folly::coro::blockingWait(
      kv::WithTransaction(getRetryStrategy()).run(kvEngine_->createReadonlyTransaction(), std::move(handler)));
}

Result<Void> MgmtdConfigFetcher::startServer(MgmtdServer &server, const flat::AppInfo &appInfo) {
  return server.start(appInfo, std::move(kvEngine_));
}
}  // namespace hf3fs::mgmtd
