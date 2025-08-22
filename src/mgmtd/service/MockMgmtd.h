#pragma once

#include <folly/Expected.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/ThreadPoolExecutor.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>
#include <unistd.h>

#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "mgmtd/service/MgmtdConfig.h"
#include "mgmtd/service/MgmtdOperator.h"
#include "mgmtd/service/MgmtdService.h"

namespace hf3fs::mgmtd {

class MockMgmtd {
 public:
  struct Config : ConfigBase<Config> {
    CONFIG_OBJ(mgmtd, MgmtdConfig);
    CONFIG_ITEM(node_id, 1u);
    CONFIG_ITEM(name, "mock-mgmtd");
    CONFIG_ITEM(cluster_id, "mock-cluster");
  };

  static CoTryTask<std::unique_ptr<MockMgmtd>> create(Config config,
                                                      std::shared_ptr<kv::IKVEngine> kvEngine,
                                                      CPUExecutorGroup *exec) {
    flat::ServiceGroupInfo groupInfo;
    groupInfo.services.emplace(mgmtd::MgmtdService::kServiceName);
    groupInfo.endpoints.push_back(net::Address::from("TCP://127.0.0.1:8000").value());

    flat::AppInfo info;
    info.nodeId = flat::NodeId(config.node_id());
    info.pid = static_cast<uint32_t>(getpid());
    info.serviceGroups = {groupInfo};
    info.clusterId = config.cluster_id();

    auto env = std::make_shared<core::ServerEnv>();
    env->setAppInfo(info);
    env->setKvEngine(kvEngine);
    env->setBackgroundExecutor(exec);

    auto mgmtd = std::make_unique<MockMgmtd>();
    mgmtd->config_ = config;
    mgmtd->mgmtdOperator_ = std::make_unique<mgmtd::MgmtdOperator>(std::move(env), mgmtd->config_.mgmtd());

    mgmtd->mgmtdOperator_->start();
    co_return mgmtd;
  }

  void stop() { mgmtdOperator_.reset(); }

  std::unique_ptr<mgmtd::MgmtdService> getService() { return std::make_unique<mgmtd::MgmtdService>(*mgmtdOperator_); }

  MgmtdOperator &getOperator() { return *mgmtdOperator_; }

 private:
  Config config_;
  std::unique_ptr<mgmtd::MgmtdOperator> mgmtdOperator_;
};

}  // namespace hf3fs::mgmtd
