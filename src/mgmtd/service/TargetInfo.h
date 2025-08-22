#pragma once

#include "WithTimestamp.h"
#include "fbs/mgmtd/TargetInfo.h"

namespace hf3fs::mgmtd {
class TargetInfo : public WithTimestamp<flat::TargetInfo> {
 public:
  using Base = WithTimestamp<flat::TargetInfo>;
  using Base::Base;

  bool locationInitLoaded = false;
  std::optional<flat::NodeId> persistedNodeId;
  std::optional<uint32_t> persistedDiskIndex;

  SteadyTime importantInfoChangedTime = SteadyClock::now();
};
}  // namespace hf3fs::mgmtd
