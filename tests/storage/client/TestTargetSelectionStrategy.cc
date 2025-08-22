#include <folly/executors/CPUThreadPoolExecutor.h>

#include "client/storage/TargetSelection.h"
#include "common/utils/Duration.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::storage::client {
namespace {

TEST(TargetSelectionStrategy, LoadDistribution) {
  const uint64_t numReplicas = 3;
  const uint64_t numNodes = 180;
  const uint64_t numTargets = numNodes * 16 * 16;
  const uint64_t numChains = numTargets / 3;
  const uint64_t numBatches = 100;

  uint64_t nodeSeqNum = 0;
  uint64_t targetId = 0;
  std::unordered_map<NodeId, uint64_t> numTargetsOnNode;
  std::unordered_map<ChainId, SlimChainInfo> chainMap;

  for (uint64_t chainId = 1; chainId <= numChains; chainId++) {
    auto &chainInfo = chainMap[ChainId(chainId)];
    chainInfo.chainId = ChainId(chainId);
    chainInfo.totalNumTargets = numReplicas;

    for (uint64_t replica = 0; replica < numReplicas; replica++) {
      NodeId nodeId((nodeSeqNum++ % numNodes) + 1);
      chainInfo.servingTargets.push_back({TargetId(++targetId), nodeId});
      numTargetsOnNode[nodeId]++;
      ASSERT_LE(numTargetsOnNode[nodeId], numTargets / numNodes);
      ASSERT_LE(targetId, numTargets);
    }
  }

  auto computeIODist = [](const std::unordered_map<NodeId, uint64_t> &numIOsOnNode) {
    std::vector<uint64_t> numIOs;
    uint64_t sumIOs = 0;

    for (const auto &item : numIOsOnNode) {
      numIOs.push_back(item.second);
      sumIOs += item.second;
    }

    std::sort(numIOs.begin(), numIOs.end());
    return std::vector{
        numIOs.front(),
        numIOs.back(),
        numIOs[numIOs.size() / 2],
        sumIOs / numIOsOnNode.size(),
        numIOsOnNode.size(),
    };
  };

  for (uint64_t numIOsPerBatch : {100, 200, 400, 1000}) {
    for (uint64_t mode = 1; mode < TargetSelectionMode::EndOfMode; mode++) {
      std::unordered_map<NodeId, uint64_t> totalIOsOnNode;
      std::vector<uint64_t> avgBatchDist(5);

      for (uint64_t k = 0; k < numBatches; k++) {
        TargetSelectionOptions options;
        options.set_mode(static_cast<TargetSelectionMode>(mode));

        auto strategy = TargetSelectionStrategy::create(options);
        if (k == 0) strategy->reset();

        std::unordered_map<NodeId, uint64_t> batchIOsOnNode;

        for (uint64_t i = 0; i < numIOsPerBatch; i++) {
          ChainId chainId(folly::Random::rand32(1, numChains + 1));
          ASSERT_TRUE(!chainMap[chainId].servingTargets.empty()) << "chainId " << chainId;
          auto selectedTarget = strategy->selectTarget(chainMap[chainId]);
          ASSERT_TRUE(std::find(chainMap[chainId].servingTargets.cbegin(),
                                chainMap[chainId].servingTargets.cend(),
                                *selectedTarget) != chainMap[chainId].servingTargets.end());
          totalIOsOnNode[selectedTarget->nodeId]++;
          batchIOsOnNode[selectedTarget->nodeId]++;
        }

        auto dist = computeIODist(batchIOsOnNode);
        for (uint64_t i = 0; i < dist.size(); i++) {
          avgBatchDist[i] += dist[i];
        }
      }

      auto overallDist = computeIODist(totalIOsOnNode);

      fmt::print("\n=== numIOsPerBatch {}, TargetSelectionMode {} ===\n", numIOsPerBatch, mode);
      fmt::print("   overall :  min {:<5d} max {:<5d} median {:<5d} avg {:<5d} #nodes {:<5d}\n",
                 overallDist[0],
                 overallDist[1],
                 overallDist[2],
                 overallDist[3],
                 overallDist[4]);
      fmt::print(" per batch :  min {:<5d} max {:<5d} median {:<5d} avg {:<5d} #nodes {:<5d}\n",
                 avgBatchDist[0] / numBatches,
                 avgBatchDist[1] / numBatches,
                 avgBatchDist[2] / numBatches,
                 avgBatchDist[3] / numBatches,
                 avgBatchDist[4] / numBatches);
    }
  }
}

TEST(TargetSelectionStrategy, MultiThreads) {
  constexpr uint64_t numReplicas = 2;
  constexpr uint64_t numNodes = 180;
  constexpr uint64_t numTargets = numNodes * 16 * 16;
  constexpr uint64_t numChains = numTargets / numReplicas;
  constexpr uint64_t numIOsPerBatch = 1024;
  constexpr uint32_t numThreads = 32;

  uint64_t nodeSeqNum = 0;
  uint64_t targetId = 0;
  std::unordered_map<NodeId, uint64_t> numTargetsOnNode;
  std::unordered_map<ChainId, SlimChainInfo> chainMap;

  for (uint64_t chainId = 1; chainId <= numChains; chainId++) {
    auto &chainInfo = chainMap[ChainId(chainId)];
    chainInfo.chainId = ChainId(chainId);
    chainInfo.totalNumTargets = numReplicas;

    for (uint64_t replica = 0; replica < numReplicas; replica++) {
      NodeId nodeId((nodeSeqNum++ % numNodes) + 1);
      chainInfo.servingTargets.push_back({TargetId(++targetId), nodeId});
      numTargetsOnNode[nodeId]++;
    }
  }

  folly::CPUThreadPoolExecutor executor(numThreads);
  auto startTime = RelativeTime::now();
  std::atomic<uint64_t> finished;
  for (auto i = 0u; i < numThreads; ++i) {
    executor.add([&] {
      while (startTime + 1_s >= RelativeTime::now()) {
        TargetSelectionOptions options;
        options.set_mode(TargetSelectionMode::RoundRobin);
        auto strategy = TargetSelectionStrategy::create(options);

        for (auto j = 0u; j < numIOsPerBatch; ++j) {
          ChainId chainId{folly::Random::rand32(1, numChains + 1)};
          ASSERT_TRUE(!chainMap[chainId].servingTargets.empty()) << "chainId " << chainId;
          auto selectedTarget = strategy->selectTarget(chainMap[chainId]);
          ASSERT_OK(selectedTarget);
        }
        finished += numIOsPerBatch;
      }
    });
  }
  executor.join();
  XLOGF(WARNING, "select target {} times", finished.load());
}

}  // namespace
}  // namespace hf3fs::storage::client
