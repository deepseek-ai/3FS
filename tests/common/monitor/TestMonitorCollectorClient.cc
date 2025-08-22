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

namespace hf3fs::monitor {

namespace test {
// const std::string kDatabase = "unit_test";

// All test disabled because they need a running monitor collector server.

TEST(TestMonitorCollector, DISABLED_MonitorCollectorStartAndStop) {
  try {
    monitor::TagSet recorder_tag_a;
    recorder_tag_a.addTag("tag", "this_is_a_tag");
    CountRecorder testAdder("test_addr_count", recorder_tag_a);
    testAdder.addSample(1);
    Monitor::Config config_;
    config_.set_reporters_length(1);
    config_.reporters(0).set_type("monitor_collector");
    config_.reporters(0).monitor_collector().set_remote_ip("1.2.3.4:5678");
    Monitor::start(config_);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Monitor::stop();
  } catch (error_t e) {
    ASSERT_TRUE(false);
  }
}

TEST(TestMonitorCollector, DISABLED_CountRecorderWithMonitorCollector) {
  try {
    monitor::TagSet recorder_tag_a;
    recorder_tag_a.addTag("tag", "a");
    monitor::TagSet recorder_tag_b;
    recorder_tag_b.addTag("tag", "b");
    CountRecorder testAdder("test_addr_count");
    testAdder.addSample(1, recorder_tag_a);
    testAdder.addSample(1, recorder_tag_a);
    testAdder.addSample(1, recorder_tag_b);

    Monitor::Config config_;
    config_.set_reporters_length(1);
    config_.reporters(0).set_type("monitor_collector");
    config_.reporters(0).monitor_collector().set_remote_ip("1.2.3.4:5678");
    Monitor::start(config_);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Monitor::stop();
  } catch (error_t e) {
    ASSERT_TRUE(false);
  }
  ASSERT_TRUE(true);
}

}  // namespace test
}  // namespace hf3fs::monitor
