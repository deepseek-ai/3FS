#include <folly/Random.h>
#include <thread>
#include <vector>

#include "common/utils/RobinHood.h"
#include "common/utils/Shards.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

TEST(TestShards, Normal) {
  Shards<std::unordered_map<uint32_t, uint32_t>, 64> shards;

  constexpr auto N = 8;
  constexpr auto M = 100000;
  std::vector<std::thread> threads(N);
  for (auto &thread : threads) {
    thread = std::thread([&shards] {
      for (auto i = 0; i < M; ++i) {
        auto r = folly::Random::rand32();
        shards.withLock([&](std::unordered_map<uint32_t, uint32_t> &map) { ++map[r]; }, r);
      }
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }

  uint64_t sum = 0;
  shards.iterate([&sum](std::unordered_map<uint32_t, uint32_t> &map) {
    for (auto &[k, v] : map) {
      sum += v;
    }
  });
  ASSERT_EQ(sum, M * N);
}

}  // namespace
}  // namespace hf3fs::test
