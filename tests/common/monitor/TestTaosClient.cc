#include <chrono>
#include <gtest/gtest.h>

#include "common/monitor/Monitor.h"

namespace hf3fs::monitor::test {
namespace {

/*
TEST(TestTaosClient, Normal) {
  constexpr auto kInsertCount = 10000;
  const std::string kDatabase = "unittest";

  TaosClient::Config config;
  config.set_db(kDatabase);
  TaosClient client(config);
  ASSERT_TRUE(client.init());

  ASSERT_TRUE(client.query(fmt::format("drop database if exists {};", kDatabase)));
  ASSERT_TRUE(client.query(fmt::format("create database {} precision 'us' update 1 keep 30;", kDatabase)));

  auto start = std::chrono::steady_clock::now();
  std::vector<Sample> samples;
  samples.reserve(kInsertCount);
  for (int64_t i = 0; i < kInsertCount; ++i) {
    TagSet sample_tag;
    samples.emplace_back(Sample{"count", sample_tag, UtcClock::now() + std::chrono::microseconds(i), i});
  }
  ASSERT_TRUE(client.commit(samples));
  auto elapsed = std::chrono::steady_clock::now() - start;
  fmt::print("Write {} count samples, takes {}ms\n",
             kInsertCount,
             std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

  start = std::chrono::steady_clock::now();
  samples.clear();
  samples.reserve(kInsertCount);
  for (uint64_t i = 0; i < kInsertCount; ++i) {
    Distribution dist;
    dist.cnt = i + 1;
    dist.sum = (i + 1) * 50;
    dist.min = 0;
    dist.max = 100;
    dist.p50 = 50;
    dist.p90 = 90;
    dist.p95 = 95;
    dist.p99 = 99;
    TagSet sample_tag;
    samples.emplace_back(Sample{"latency", sample_tag, UtcClock::now() + std::chrono::microseconds(i), dist});
  }
  ASSERT_TRUE(client.commit(samples));
  elapsed = std::chrono::steady_clock::now() - start;
  fmt::print("Write {} latency samples, takes {}ms\n",
             kInsertCount,
             std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

  client.stop();
}
*/

}  // namespace
}  // namespace hf3fs::monitor::test
