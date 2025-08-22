#pragma once

#include <condition_variable>
#include <folly/Utility.h>
#include <optional>

#include "common/net/IOWorker.h"
#include "common/net/Listener.h"
#include "common/net/Processor.h"
#include "common/net/ThreadPoolGroup.h"
#include "common/serde/ClientContext.h"
#include "common/serde/Services.h"
#include "common/utils/Address.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"

namespace hf3fs::net {

class ServiceGroup : public folly::MoveOnly {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_ITEM(services, std::set<std::string>{});
    CONFIG_ITEM(use_independent_thread_pool, false);
    CONFIG_ITEM(network_type, Address::RDMA);
    CONFIG_HOT_UPDATED_ITEM(check_connections_interval, 60_s);
    CONFIG_HOT_UPDATED_ITEM(connection_expiration_time, 1_d);
    CONFIG_OBJ(io_worker, IOWorker::Config);
    CONFIG_OBJ(processor, Processor::Config);
    CONFIG_OBJ(listener, Listener::Config);
  };

  explicit ServiceGroup(const Config &config, ThreadPoolGroup &tpg);

  ~ServiceGroup();

  // add normal service.
  template <class Service>
  Result<Void> addSerdeService(std::unique_ptr<Service> &&obj, std::optional<Address::Type> type = {}) {
    return serdeServices_.addService(std::move(obj), type.value_or(config_.network_type()) == Address::RDMA);
  }

  // setup this group.
  Result<Void> setup();

  // start this group.
  Result<Void> start();

  // stop and wait.
  void stopAndJoin();

  // set processor frozen.
  void setFrozen(bool frozen = true) { processor_.setFrozen(frozen, config_.network_type() == Address::RDMA); }

  // get listening address list.
  auto &addressList() const { return listener_.addressList(); }

  // get service name list.
  auto &serviceNameList() const { return config_.services(); }

  // get io worker.
  auto &ioWorker() { return ioWorker_; }

  // set coroutines pool getter for processor.
  void setCoroutinesPoolGetter(auto &&g) { processor_.setCoroutinesPoolGetter(std::forward<decltype(g)>(g)); }

 protected:
  CoTask<void> checkConnectionsRegularly();

 private:
  ConstructLog<"net::ServiceGroup"> constructLog_;
  const Config &config_;
  ThreadPoolGroup &tpg_;
  serde::Services serdeServices_;
  Processor processor_;
  IOWorker ioWorker_;
  Listener listener_;

  CancellationSource cancel_;
  folly::SemiFuture<Void> future_;
};

}  // namespace hf3fs::net
