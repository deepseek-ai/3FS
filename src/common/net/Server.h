#pragma once

#include <algorithm>
#include <folly/Random.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <vector>

#include "common/app/AppInfo.h"
#include "common/net/ServiceGroup.h"
#include "common/net/ThreadPoolGroup.h"
#include "common/serde/ClientContext.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Result.h"

namespace hf3fs::net {

class Server {
 public:
  static constexpr auto kName = "Server";

  struct Config : public ConfigBase<Config> {
    CONFIG_OBJ(thread_pool, ThreadPoolGroup::Config);
    CONFIG_OBJ(independent_thread_pool, ThreadPoolGroup::Config);
    CONFIG_OBJ_ARRAY(groups, ServiceGroup::Config, 4);
  };

  Server(const Config &config);
  virtual ~Server() { stopAndJoin(); }

  // get service groups.
  auto &groups() { return groups_; }
  auto &groups() const { return groups_; }

  auto serdeCtxCreator() {
    return [this](Address addr) { return serde::ClientContext(groups_.front()->ioWorker(), addr, options_); };
  }

  // add service into group.
  template <class Service>
  Result<Void> addSerdeService(std::unique_ptr<Service> &&obj, bool strict = false) {
    for (auto &group : groups_) {
      if (group->serviceNameList().contains(std::string{Service::kServiceName})) {
        return group->addSerdeService(std::move(obj));
      }
    }
    if (strict) {
      return makeError(RPCCode::kInvalidServiceName);
    }
    return groups_.front()->addSerdeService(std::move(obj));
  }

  // setup the server.
  Result<Void> setup();

  // start the server.
  Result<Void> start(const flat::AppInfo &info = {});

  // stop the server.
  void stopAndJoin();

  // set processor frozen.
  void setFrozen(bool frozen = true);

  // get the primary thread pool group.
  auto &tpg() { return tpg_; }

  // get the app info.
  const auto &appInfo() const { return appInfo_; }

  // describe the server.
  std::string describe() const;

  std::vector<flat::ServiceGroupInfo> getServiceGroupInfos() const;

 protected:
  // call it before start.
  virtual Result<Void> beforeStart() { return Void{}; }

  // call it after start.
  virtual Result<Void> afterStart() { return Void{}; }

  // call it before stop.
  virtual Result<Void> beforeStop() { return Void{}; }

  // call it after stop.
  virtual Result<Void> afterStop() { return Void{}; }

 private:
  ConstructLog<"net::Server"> constructLog_;
  const Config &config_;
  ThreadPoolGroup tpg_;
  ThreadPoolGroup independentTpg_;
  std::vector<std::unique_ptr<ServiceGroup>> groups_;
  flat::AppInfo appInfo_;
  std::atomic_flag stopped_;
  folly::atomic_shared_ptr<const CoreRequestOptions> options_{std::make_shared<CoreRequestOptions>()};
};

}  // namespace hf3fs::net
