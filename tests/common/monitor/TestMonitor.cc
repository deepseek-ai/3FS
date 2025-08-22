#include <chrono>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <ratio>
#include <thread>
#include <vector>

#include "common/monitor/Monitor.h"
#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"

namespace hf3fs::monitor {

bool checkRecorderHasTag(const Recorder &rec, const TagSet &tag) { return rec.map_.find(tag) != rec.map_.end(); }

void printSamples(const std::vector<Sample> &samples) {
  XLOGF(INFO, "{} Samples:", samples.size());
  for (auto &sample : samples) {
    if (sample.isNumber()) {
      XLOGF(INFO, "\t{}: {}", sample.name, sample.number());
    } else if (sample.isDistribution()) {
      auto &dist = sample.dist();
      XLOGF(INFO,
            "\t{}: cnt {} sum {} min {} p50 {} p90 {} p95 {} p99 {} max {}",
            sample.name,
            dist.cnt,
            dist.sum,
            dist.min,
            dist.p50,
            dist.p90,
            dist.p95,
            dist.p99,
            dist.max);
    }
  }
}

namespace test {

size_t countMatchedSamples(const std::string &counterPrefix, const std::vector<Sample> &samples) {
  size_t sampleSize = 0;
  for (const auto &s : samples)
    if (s.name.starts_with(counterPrefix)) sampleSize++;
  return sampleSize;
}

size_t filterSamples(const std::string &counterPrefix, std::vector<Sample> &samples) {
  std::vector<Sample> matchedSamples;
  for (const auto &s : samples)
    if (s.name.starts_with(counterPrefix)) matchedSamples.push_back(s);
  matchedSamples.swap(samples);
  return samples.size();
}

TEST(TestMonitor, StartAndStop) {
  Monitor::Config config;
  Monitor::start(config);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Monitor::stop();
}

TEST(TestMonitor, CountRecorder) {
  auto &collector = Monitor::getDefaultInstance().getCollector();
  CountRecorder testAdder("test_addr");
  CountRecorder tagAdderA("tag_adder", monitor::TagSet{{"tag", "a"}});
  CountRecorder tagAdderB("tag_adder", monitor::TagSet{{"tag", "b"}});

  constexpr auto N = 8;
  constexpr auto M = 1000000;
  std::vector<std::thread> threads(N);
  for (auto &thread : threads) {
    thread = std::thread([&] {
      for (auto m = 0; m < M; ++m) {
        testAdder.addSample(1);
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  std::vector<Sample> samples;
  testAdder.collect(samples);
  ASSERT_EQ(countMatchedSamples(testAdder.name(), samples), 1);
  filterSamples(testAdder.name(), samples);
  ASSERT_EQ(samples.front().number(), N * M);

  // TODO_221013
  monitor::TagSet recorder_tag_a;
  recorder_tag_a.addTag("tag", "a");
  monitor::TagSet recorder_tag_b;
  recorder_tag_b.addTag("tag", "b");
  testAdder.addSample(1, recorder_tag_a);
  testAdder.addSample(1, recorder_tag_b);
  samples.clear();
  collector.collectAll(0, samples, true);
  ASSERT_EQ(countMatchedSamples(testAdder.name(), samples), 2);
  ASSERT_TRUE(checkRecorderHasTag(testAdder, recorder_tag_a));
  ASSERT_TRUE(checkRecorderHasTag(testAdder, recorder_tag_b));

  testAdder.addSample(1, recorder_tag_b);
  samples.clear();
  collector.collectAll(0, samples, true);
  ASSERT_EQ(countMatchedSamples(testAdder.name(), samples), 1);
  printSamples(samples);
  ASSERT_FALSE(checkRecorderHasTag(testAdder, recorder_tag_a));
  ASSERT_TRUE(checkRecorderHasTag(testAdder, recorder_tag_b));

  testAdder.addSample(1, recorder_tag_a);
  testAdder.addSample(1, recorder_tag_b);
  samples.clear();
  collector.collectAll(0, samples, false);
  printSamples(samples);

  monitor::TagSet recorder_tag_1{{"anothertag", "1"}};
  monitor::TagSet recorder_tag_2{{"anothertag", "2"}};
  tagAdderA.addSample(1, recorder_tag_1);
  tagAdderA.addSample(1, recorder_tag_2);
  samples.clear();
  collector.collectAll(0, samples, true);
  ASSERT_EQ(countMatchedSamples(tagAdderA.name(), samples), 2);
  tagAdderB.addSample(1, recorder_tag_1);
  tagAdderB.addSample(1, recorder_tag_2);
  samples.clear();
  collector.collectAll(0, samples, true);
  ASSERT_EQ(countMatchedSamples(tagAdderB.name(), samples), 2);
  tagAdderA.addSample(1, recorder_tag_1);
  tagAdderB.addSample(1, recorder_tag_2);
  samples.clear();
  collector.collectAll(0, samples, true);
  ASSERT_EQ(countMatchedSamples(tagAdderA.name(), samples), 2);
  tagAdderA.addSample(1, recorder_tag_1);
  tagAdderA.addSample(1, recorder_tag_2);
  tagAdderB.addSample(1, recorder_tag_1);
  tagAdderB.addSample(1, recorder_tag_2);
  samples.clear();
  collector.collectAll(0, samples, true);
  ASSERT_EQ(countMatchedSamples(tagAdderA.name(), samples), 4);
}

TEST(TestMonitor, LatencyRecorder) {
  auto &collector = Monitor::getDefaultInstance().getCollector();
  LatencyRecorder testLatency("test_latency");
  LatencyRecorder tagLatencyA("tag_latency", monitor::TagSet{{"tag", "a"}});
  LatencyRecorder tagLatencyB("tag_latency", monitor::TagSet{{"tag", "b"}});

  constexpr auto N = 10;
  constexpr auto M = 1000000;
  std::vector<std::thread> threads(N);
  int cnt = 0;
  for (auto &thread : threads) {
    thread = std::thread([&, duration = cnt++ * M] {
      for (auto m = 0; m < M; ++m) {
        testLatency.addSample(std::chrono::nanoseconds(duration + m));
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  std::vector<Sample> samples;
  testLatency.collect(samples);
  ASSERT_EQ(countMatchedSamples(testLatency.name(), samples), 1);
  auto &sample = samples.front();
  ASSERT_TRUE(sample.isDistribution());

  ASSERT_EQ(sample.dist().cnt, N * M);
  ASSERT_NEAR(sample.dist().mean(), N * M / 2.0, 1e1);
  ASSERT_NEAR(sample.dist().min, 0, 1e-4);
  ASSERT_NEAR(sample.dist().max, N * M - 1.0, 1e-4);
  ASSERT_NEAR(sample.dist().p90, N * M * 0.9, 1e4);
  ASSERT_NEAR(sample.dist().p99, N * M * 0.99, 1e4);

  samples.clear();
  testLatency.collect(samples);
  ASSERT_EQ(countMatchedSamples(testLatency.name(), samples), 0);

  // TODO_221013
  monitor::TagSet recorder_tag_a;
  recorder_tag_a.addTag("tag", "a");
  monitor::TagSet recorder_tag_b;
  recorder_tag_b.addTag("tag", "b");
  testLatency.addSample(std::chrono::nanoseconds(1), recorder_tag_a);
  testLatency.addSample(std::chrono::nanoseconds(1), recorder_tag_b);
  samples.clear();
  collector.collectAll(0, samples, true);
  ASSERT_EQ(countMatchedSamples(testLatency.name(), samples), 2);
  printSamples(samples);
  ASSERT_TRUE(checkRecorderHasTag(testLatency, recorder_tag_a));
  ASSERT_TRUE(checkRecorderHasTag(testLatency, recorder_tag_b));

  testLatency.addSample(std::chrono::nanoseconds(1), recorder_tag_b);
  samples.clear();
  collector.collectAll(0, samples, true);
  ASSERT_EQ(countMatchedSamples(testLatency.name(), samples), 1);
  ASSERT_FALSE(checkRecorderHasTag(testLatency, recorder_tag_a));
  ASSERT_TRUE(checkRecorderHasTag(testLatency, recorder_tag_b));

  testLatency.addSample(std::chrono::nanoseconds(1), recorder_tag_a);
  testLatency.addSample(std::chrono::nanoseconds(1), recorder_tag_b);
  samples.clear();
  collector.collectAll(0, samples, false);
  printSamples(samples);

  monitor::TagSet recorder_tag_1{{"anothertag", "1"}};
  monitor::TagSet recorder_tag_2{{"anothertag", "2"}};
  tagLatencyA.addSample(std::chrono::nanoseconds(1), recorder_tag_1);
  tagLatencyA.addSample(std::chrono::nanoseconds(1), recorder_tag_2);
  samples.clear();
  collector.collectAll(0, samples, true);
  ASSERT_EQ(countMatchedSamples(tagLatencyA.name(), samples), 2);
  tagLatencyB.addSample(std::chrono::nanoseconds(1), recorder_tag_1);
  tagLatencyB.addSample(std::chrono::nanoseconds(1), recorder_tag_2);
  samples.clear();
  collector.collectAll(0, samples, true);
  ASSERT_EQ(countMatchedSamples(tagLatencyB.name(), samples), 2);
  tagLatencyA.addSample(std::chrono::nanoseconds(1), recorder_tag_1);
  tagLatencyB.addSample(std::chrono::nanoseconds(1), recorder_tag_2);
  samples.clear();
  collector.collectAll(0, samples, true);
  ASSERT_EQ(countMatchedSamples(tagLatencyA.name(), samples), 2);
  tagLatencyA.addSample(std::chrono::nanoseconds(1), recorder_tag_1);
  tagLatencyA.addSample(std::chrono::nanoseconds(1), recorder_tag_2);
  tagLatencyB.addSample(std::chrono::nanoseconds(1), recorder_tag_1);
  tagLatencyB.addSample(std::chrono::nanoseconds(1), recorder_tag_2);
  samples.clear();
  collector.collectAll(0, samples, true);
  ASSERT_EQ(countMatchedSamples(tagLatencyA.name(), samples), 4);
}

TEST(TestMonitor, DISABLED_abort_on_duplicate) {
  LatencyRecorder recorder1("tag_latency", monitor::TagSet{{"tag", "a"}});
  LatencyRecorder recorder2("tag_latency", monitor::TagSet{{"tag", "b"}});
  // abort here.
  LatencyRecorder recorder3("tag_latency", monitor::TagSet{{"tag", "b"}});
}

}  // namespace test
}  // namespace hf3fs::monitor
