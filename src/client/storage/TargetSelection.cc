#include "TargetSelection.h"

#include <folly/AtomicUnorderedMap.h>

namespace hf3fs::storage::client {
namespace {

folly::AtomicUnorderedInsertMap<NodeId, folly::MutableAtom<uint64_t>> numAccumIOs{32_KB};
folly::AtomicUnorderedInsertMap<ChainId, folly::MutableAtom<uint64_t>> nextTargetIndex{1_MB};

}  // namespace

// Try to distribute the read IOs evenly over all storage nodes
class LoadBalanceStrategy : public TargetSelectionStrategy {
 public:
  LoadBalanceStrategy(const TargetSelectionOptions &options)
      : TargetSelectionStrategy(options),
        numIOs(kNodeIdKeyedMapExpectedNumElements) {}

  Result<SlimTargetInfo> selectTarget(const SlimChainInfo &chain) override {
    uint32_t targetIndex = folly::Random::rand32(0, chain.servingTargets.size());
    SlimTargetInfo preferredTarget = chain.servingTargets[targetIndex];
    auto [preferredIt, succ] = numAccumIOs.emplace(preferredTarget.nodeId, 0);

    for (const auto &target : chain.servingTargets) {
      if (numIOs[target.nodeId] < numIOs[preferredTarget.nodeId]) {
        preferredTarget = target;
      } else if (target.nodeId != preferredTarget.nodeId && numIOs[target.nodeId] == numIOs[preferredTarget.nodeId]) {
        auto [currentIt, succ1] = numAccumIOs.emplace(target.nodeId, 0);
        if (currentIt->second.data < preferredIt->second.data) {
          preferredTarget = target;
          std::memcpy(&preferredIt, &currentIt, sizeof(preferredIt));
        }
      }
    }

    numIOs[preferredTarget.nodeId]++;
    preferredIt->second.data++;

    return preferredTarget;
  }

  bool selectAnyTarget() override { return true; }

  void reset() override {
    for (auto &[k, v] : numAccumIOs) {
      v.data = 0;
    }
  }

 private:
  std::unordered_map<NodeId, uint64_t> numIOs;
};

// Select storage targets on the chain in a round-robin fashion
class RoundRobinStrategy : public TargetSelectionStrategy {
 public:
  RoundRobinStrategy(const TargetSelectionOptions &options)
      : TargetSelectionStrategy(options) {}

  Result<SlimTargetInfo> selectTarget(const SlimChainInfo &chain) override {
    auto [it, succ] = nextTargetIndex.emplace(chain.chainId, 0);
    auto targetIndex = (it->second.data++) % chain.servingTargets.size();
    return chain.servingTargets[targetIndex];
  }

  bool selectAnyTarget() override { return true; }

  void reset() override {
    for (auto &[k, v] : nextTargetIndex) {
      v.data = 0;
    }
  }
};

// Select a random storage target on the chain
class RandomTargetStrategy : public TargetSelectionStrategy {
 public:
  RandomTargetStrategy(const TargetSelectionOptions &options)
      : TargetSelectionStrategy(options) {}

  Result<SlimTargetInfo> selectTarget(const SlimChainInfo &chain) override {
    uint32_t targetIndex = folly::Random::rand32(0, chain.servingTargets.size());
    return chain.servingTargets[targetIndex];
  }

  bool selectAnyTarget() override { return true; }
};

// Always select the tail target
class TailTargetStrategy : public TargetSelectionStrategy {
 public:
  TailTargetStrategy(const TargetSelectionOptions &options)
      : TargetSelectionStrategy(options) {}

  Result<SlimTargetInfo> selectTarget(const SlimChainInfo &chain) override { return chain.servingTargets.back(); }
};

// Always select the head target
class HeadTargetStrategy : public TargetSelectionStrategy {
 public:
  HeadTargetStrategy(const TargetSelectionOptions &options)
      : TargetSelectionStrategy(options) {}

  Result<SlimTargetInfo> selectTarget(const SlimChainInfo &chain) override { return chain.servingTargets.front(); }
};

// Always select the head target
class ManualModeStrategy : public TargetSelectionStrategy {
 public:
  ManualModeStrategy(const TargetSelectionOptions &options)
      : TargetSelectionStrategy(options) {}

  Result<SlimTargetInfo> selectTarget(const SlimChainInfo &chain) override {
    if (options_.targetIndex() >= chain.servingTargets.size()) {
      XLOGF(CRITICAL,
            "Target index chosen by user is out of boundary: {} >= {}",
            options_.targetIndex(),
            chain.servingTargets.size());
      return makeError(StorageClientCode::kNotAvailable);
    }

    return chain.servingTargets[options_.targetIndex()];
  }
};

std::unique_ptr<TargetSelectionStrategy> TargetSelectionStrategy::create(const TargetSelectionOptions &options) {
  switch (options.mode()) {
    case TargetSelectionMode::LoadBalance:
      return std::make_unique<LoadBalanceStrategy>(options);

    case TargetSelectionMode::RoundRobin:
      return std::make_unique<RoundRobinStrategy>(options);

    case TargetSelectionMode::RandomTarget:
      return std::make_unique<RandomTargetStrategy>(options);

    case TargetSelectionMode::TailTarget:
      return std::make_unique<TailTargetStrategy>(options);

    case TargetSelectionMode::HeadTarget:
      return std::make_unique<HeadTargetStrategy>(options);

    case TargetSelectionMode::ManualMode:
      return std::make_unique<ManualModeStrategy>(options);

    default:
      return nullptr;
  }
}

}  // namespace hf3fs::storage::client
