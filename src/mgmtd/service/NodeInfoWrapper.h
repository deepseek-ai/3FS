#pragma once

#include "WithTimestamp.h"
#include "common/utils/SimpleRingBuffer.h"
#include "fbs/mgmtd/NodeInfo.h"
#include "fbs/mgmtd/NodeStatusChange.h"

namespace hf3fs::mgmtd {
class NodeInfoWrapper : public WithTimestamp<flat::NodeInfo> {
 public:
  using Base = WithTimestamp<flat::NodeInfo>;
  using Base::Base;

  void updateHeartbeatVersion(flat::HeartbeatVersion version) { version_ = version; }
  flat::HeartbeatVersion lastHbVersion() const { return version_; }

  void recordStatusChange(flat::NodeStatus status, UtcTime now) {
    if (statusChanges_.full()) statusChanges_.pop();
    statusChanges_.push(flat::NodeStatusChange::create(status, now));
  }

  std::vector<flat::NodeStatusChange> getAllStatusChanges() {
    return std::vector<flat::NodeStatusChange>(statusChanges_.begin(), statusChanges_.end());
  }

 private:
  flat::HeartbeatVersion version_{0};  // used for rejecting stale heartbeat requests
  static constexpr size_t kStatusChangeCount = 100;

  SimpleRingBuffer<flat::NodeStatusChange> statusChanges_{kStatusChangeCount};
};
}  // namespace hf3fs::mgmtd
