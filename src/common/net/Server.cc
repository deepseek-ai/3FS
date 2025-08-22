#include "common/net/Server.h"

#include <fmt/core.h>
#include <iterator>

#include "common/net/ib/IBDevice.h"
#include "common/utils/LogCommands.h"

namespace hf3fs::net {

Server::Server(const Config &config)
    : config_(config),
      tpg_("Svr", config_.thread_pool()),
      independentTpg_("SvrI", config_.independent_thread_pool()) {
  auto serviceNum = config_.groups_length();
  groups_.reserve(serviceNum);
  for (auto i = 0ul; i < serviceNum; ++i) {
    auto &tpg = config_.groups(i).use_independent_thread_pool() ? independentTpg_ : tpg_;
    groups_.emplace_back(std::make_unique<ServiceGroup>(config_.groups(i), tpg));
  }
}

Result<Void> Server::setup() {
  auto setupFunc = [&]() -> Result<Void> {
    for (auto &group : groups_) {
      auto desc = fmt::format("Setup group ({})", fmt::join(group->serviceNameList(), ","));
      RETURN_ON_ERROR_LOG_WRAPPED(INFO, desc, group->setup());
    }
    return Void{};
  };
  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Server::setup", setupFunc());
  return Void{};
}

Result<Void> Server::start(const flat::AppInfo &appInfo) {
  appInfo_ = appInfo;
  appInfo_.serviceGroups = getServiceGroupInfos();

  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Server::beforeStart", beforeStart());
  for (auto &group : groups_) {
    auto desc = fmt::format("Start group ({})", fmt::join(group->serviceNameList(), ","));
    RETURN_ON_ERROR_LOG_WRAPPED(INFO, desc, group->start());
  }
  RETURN_ON_ERROR_LOG_WRAPPED(INFO, "Server::afterStart", afterStart());
  return Void{};
}

void Server::stopAndJoin() {
  if (stopped_.test_and_set()) {
    XLOGF(INFO, "Server {} is already stopped", fmt::ptr(this));
    return;
  }

  LOG_COMMAND(INFO, "Server::beforeStop", beforeStop());

  for (auto &group : groups_) {
    auto desc = fmt::format("Stop group ({})", fmt::join(group->serviceNameList(), ","));
    LOG_COMMAND(INFO, desc, group->stopAndJoin());
  }

  LOG_COMMAND(INFO, "Server::afterStop", afterStop());
  LOG_COMMAND(INFO, "Stop tpg", tpg_.stopAndJoin());
  LOG_COMMAND(INFO, "Stop independentTpg", independentTpg_.stopAndJoin());
}

void Server::setFrozen(bool frozen) {
  for (auto &group : groups_) {
    group->setFrozen(frozen);
  }
}

std::string Server::describe() const {
  std::string buffer;
  fmt::format_to(std::back_inserter(buffer), "Server {} has {} groups:\n", fmt::ptr(this), groups_.size());
  int idx = 0;
  for (auto &group : groups_) {
    fmt::format_to(std::back_inserter(buffer), "Group {}\n", idx++);
    for (const auto &service : group->serviceNameList()) {
      fmt::format_to(std::back_inserter(buffer), "  Service: {}\n", service);
    }
    for (const auto &addr : group->addressList()) {
      fmt::format_to(std::back_inserter(buffer), "  Listening: {}\n", addr);
    }
  }
  return buffer;
}

std::vector<flat::ServiceGroupInfo> Server::getServiceGroupInfos() const {
  std::vector<flat::ServiceGroupInfo> infos;
  for (auto &group : groups()) {
    infos.emplace_back(group->serviceNameList(), group->addressList());
  }

  return infos;
}

}  // namespace hf3fs::net
