#pragma once

#include "common/utils/StringUtils.h"
#include "mgmtd/MgmtdServer.h"
#include "mgmtd/background/MgmtdLeaseExtender.h"
#include "mgmtd/service/MgmtdOperator.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd::testing {

class MgmtdTestHelper {
 public:
  MgmtdTestHelper(MgmtdOperator &op)
      : op_(op) {}
  MgmtdTestHelper(MgmtdServer &server)
      : MgmtdTestHelper(*server.mgmtdOperator_) {}

  MgmtdStore &getStore() { return op_.state_.store_; }
  kv::IKVEngine &getEngine() { return *op_.state_.env_->kvEngine(); }

  CoTask<NodeInfoWrapper *> getNodeInfo(flat::NodeId id) {
    auto dataPtr = co_await op_.state_.data_.coLock();
    auto &nm = dataPtr->routingInfo.nodeMap;
    assert(nm.contains(id));
    co_return &nm.at(id);
  }

  CoTask<void> extendLease() { co_await op_.backgroundRunner_.leaseExtender_->extend(); }

  CoTryTask<void> setRoutingInfo(std::function<void(flat::RoutingInfo &)> callback);

 private:
  Result<Void> checkDataIntegrity(const flat::RoutingInfo &ri);

  MgmtdOperator &op_;
};
}  // namespace hf3fs::mgmtd::testing
