#pragma once

#include <algorithm>
#include <cassert>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/Utility.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/logging/xlog.h>
#include <map>
#include <memory>
#include <unistd.h>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/app/NodeId.h"
#include "common/kv/IKVEngine.h"
#include "common/kv/mem/MemKVEngine.h"
#include "common/serde/ClientMockContext.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "meta/components/ChainAllocator.h"
#include "meta/components/FileHelper.h"
#include "meta/components/GcManager.h"
#include "meta/service/MetaOperator.h"
#include "meta/service/MetaSerdeService.h"
#include "meta/store/MetaStore.h"

namespace hf3fs::meta::server {

class MockMeta : folly::NonCopyableNonMovable {
 public:
  static CoTryTask<std::unique_ptr<MockMeta>> create(const Config &cfg,
                                                     std::shared_ptr<kv::IKVEngine> kv,
                                                     std::shared_ptr<client::ICommonMgmtdClient> mgmtdClient) {
    auto meta = std::unique_ptr<MockMeta>(new MockMeta(cfg, kv, mgmtdClient));
    for (auto &moperator : meta->operators_) {
      CO_RETURN_ON_ERROR(co_await moperator->init(Layout::newEmpty(ChainTableId(1), 512 << 10, 128)));
    }
    co_return meta;
  }

  ~MockMeta() { stop(); }

  void start(CPUExecutorGroup &exec) {
    for (auto &moperator : operators_) {
      moperator->start(exec);
    }
  }

  void stop() {
    for (auto &moperator : operators_) {
      moperator->beforeStop();
      moperator->afterStop();
    }
  }

  std::unique_ptr<MetaSerdeService> getService() { return std::make_unique<MetaSerdeService>(*operators_.at(0)); }

  MetaOperator &getOperator() { return *operators_.at(0); }

  MetaStore &getStore() { return dynamic_cast<MetaStore &>(*getOperator().metaStore_); }

  storage::client::StorageClient &getStorageClient() { return *storageClient_; }

  FileHelper &getFileHelper() { return *getOperator().fileHelper_; }

  GcManager &getGcManager() { return *getOperator().gcManager_; }

  SessionManager &getSessionManager() { return *getOperator().sessionManager_; }

 private:
  MockMeta(const Config &cfg,
           std::shared_ptr<kv::IKVEngine> kv,
           std::shared_ptr<client::ICommonMgmtdClient> mgmtdClient)
      : cfg_(cfg),
        mgmtdClient_(mgmtdClient) {
    storageClientCfg_.set_implementation_type(storage::client::StorageClient::ImplementationType::InMem);
    storageClient_ = storage::client::StorageClient::create(ClientId::random(), storageClientCfg_, *mgmtdClient_);

    auto routing = mgmtdClient->getRoutingInfo();
    XLOGF_IF(FATAL, !routing, "routing info not available");
    auto nodes = routing->getNodeBy(flat::selectNodeByType(flat::NodeType::META) && flat::selectActiveNode());
    XLOGF_IF(FATAL, nodes.empty(), "no active metas");
    for (auto &node : nodes) {
      XLOGF_IF(FATAL, contexts_.contains(node.app.nodeId), "duplicated {}", node.app.nodeId);
      auto moperator = std::make_unique<MetaOperator>(
          cfg,
          node.app.nodeId,
          kv,
          mgmtdClient_,
          storageClient_,
          std::make_unique<Forward>(cfg.forward(), node.app.nodeId, contexts_, mgmtdClient_));
      contexts_[node.app.nodeId] = serde::ClientMockContext::create(std::make_unique<MetaSerdeService>(*moperator));
      operators_.push_back(std::move(moperator));
    }
  }

  [[maybe_unused]] const Config &cfg_;
  storage::client::StorageClient::Config storageClientCfg_;
  std::vector<std::unique_ptr<MetaOperator>> operators_;
  std::map<flat::NodeId, serde::ClientMockContext> contexts_;
  std::shared_ptr<client::ICommonMgmtdClient> mgmtdClient_;
  std::shared_ptr<storage::client::StorageClient> storageClient_;
};

}  // namespace hf3fs::meta::server
