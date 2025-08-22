#pragma once

#include <folly/Random.h>
#include <mutex>
#include <vector>

#include "common/utils/StatusCodeDetails.h"
#include "fbs/storage/Common.h"

namespace hf3fs::storage::client {

/* Slim routing info */

struct SlimTargetInfo {
  TargetId targetId;
  NodeId nodeId;

  bool operator==(const SlimTargetInfo &other) const = default;
};

struct SlimChainInfo {
  ChainId chainId;
  ChainVer version;
  flat::RoutingInfoVersion routingInfoVer;
  size_t totalNumTargets;  // total number of targets including non-serving targets
  std::vector<SlimTargetInfo> servingTargets;
};

enum TargetSelectionMode {
  Default = 0,
  LoadBalance,
  RoundRobin,
  RandomTarget,
  TailTarget,
  HeadTarget,
  ManualMode,
  EndOfMode
};

class TargetSelectionOptions : public hf3fs::ConfigBase<TargetSelectionOptions> {
  // if mode = Tail/Head, but tail/head is not in the specified traffic zone, the read could fail
  // if mode = LB/RR/Random, only storage targets hosted in the specified traffic zone are selected
  CONFIG_HOT_UPDATED_ITEM(mode, TargetSelectionMode::Default);
  CONFIG_HOT_UPDATED_ITEM(targetIndex, 0U);  // the target chosen by user in ManualMode
  CONFIG_HOT_UPDATED_ITEM(trafficZone, "");
};

// An instance of each TargetSelectionStrategy implementation is created for each batch read.
class TargetSelectionStrategy {
 public:
  TargetSelectionStrategy(const TargetSelectionOptions &options)
      : options_(options) {}

  virtual ~TargetSelectionStrategy() = default;

  // selectTarget() is called for each read IO in the batch.
  virtual Result<SlimTargetInfo> selectTarget(const SlimChainInfo &chain) = 0;

  virtual bool selectAnyTarget() { return false; }

  // reset the internal states
  virtual void reset() {}

  static std::unique_ptr<TargetSelectionStrategy> create(const TargetSelectionOptions &options);

 protected:
  static constexpr size_t kNodeIdKeyedMapExpectedNumElements = 1000;
  TargetSelectionOptions options_;
};

}  // namespace hf3fs::storage::client
