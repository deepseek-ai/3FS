#include <fmt/core.h>
#include <folly/dynamic.h>
#include <folly/init/Init.h>
#include <folly/json.h>
#include <folly/logging/LogConfigParser.h>
#include <folly/logging/xlog.h>
#include <folly/synchronization/LifoSem.h>
#include <gflags/gflags.h>
#include <gflags/gflags_declare.h>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>

#include "common/logging/LogInit.h"
#include "common/monitor/Monitor.h"
#include "common/net/ib/IBDevice.h"
#include "common/utils/SysResource.h"
#include "memory/common/OverrideCppNewDelete.h"

DECLARE_string(logging);
DEFINE_string(log_to_directory, "", "path of log directory, each testsuit log to a separate file");
DEFINE_bool(monitor, false, "use log monitor");

DEFINE_string(timeout, "", "");

class TestLoggerSetter : public ::testing::TestEventListener {
 public:
  void OnTestIterationStart(const ::testing::UnitTest &, int iteration) override { iteration_ = iteration; }

#define PLOGF(LEVEL, formatStr, ...)                                                                               \
  XLOGF(LEVEL, formatStr __VA_OPT__(, ) __VA_ARGS__);                                                              \
  fmt::print("[{:%Y-%m-%d %H:%M:%S}] " formatStr "\n",                                                             \
             fmt::localtime(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())) __VA_OPT__(, ) \
                 __VA_ARGS__);

  // Fired before the test starts.
  void OnTestStart(const ::testing::TestInfo &test_info) override {
    setFollyLog(test_info.test_suite_name());
    XLOGF(INFO, "-------------------------------------------------------------------------");
    std::string buf;
    fmt::format_to(std::back_inserter(buf),
                   "Test Start: {}.{}, run {}, iteration {}, file {}:{}",
                   test_info.test_suite_name(),
                   test_info.name(),
                   test_info.should_run(),
                   iteration_,
                   test_info.file(),
                   test_info.line());
    if (test_info.type_param()) {
      fmt::format_to(std::back_inserter(buf), ", type_param {}", test_info.type_param());
    }
    if (test_info.value_param()) {
      fmt::format_to(std::back_inserter(buf), ", value_param {}", test_info.value_param());
    }
    PLOGF(INFO, "{}", buf);
    XLOGF(INFO, "-------------------------------------------------------------------------");
  }

  void OnTestEnd(const ::testing::TestInfo &test_info) override {
    XLOGF(INFO, "-------------------------------------------------------------------------");
    auto status = "[ OK ]";
    if (test_info.result()->Failed()) {
      status = "[ FAILED ]";
    } else if (test_info.result()->Skipped()) {
      status = "[ SKIPPED ]";
    }
    PLOGF(INFO, "Test End: {}.{}, {}", test_info.test_suite_name(), test_info.name(), status);
    XLOGF(INFO, "-------------------------------------------------------------------------");
    clearEmptyLog(test_info.test_suite_name());
  }

  // Fired before any test activity starts.
  void OnTestProgramStart(const ::testing::UnitTest &) override {}

  // Fired before environment set-up for each iteration of tests starts.
  void OnEnvironmentsSetUpStart(const ::testing::UnitTest &) override {}

  // Fired after environment set-up for each iteration of tests ends.
  void OnEnvironmentsSetUpEnd(const ::testing::UnitTest &) override {}

  // Fired after a failed assertion or a SUCCEED() invocation.
  // If you want to throw an exception from this function to skip to the next
  // TEST, it must be AssertionException defined above, or inherited from it.
  void OnTestPartResult(const ::testing::TestPartResult &test_part_result) override {
    PLOGF(INFO, "TestPartResult: {}", test_part_result.summary());
  }

  // Fired after the test suite ends.`
  void OnTestSuiteEnd(const ::testing::TestSuite & /*test_suite*/) override {}

  // Fired before environment tear-down for each iteration of tests starts.
  void OnEnvironmentsTearDownStart(const ::testing::UnitTest &) override {}

  // Fired after environment tear-down for each iteration of tests ends.
  void OnEnvironmentsTearDownEnd(const ::testing::UnitTest &) override {}

  // Fired after each iteration of tests finishes.
  void OnTestIterationEnd(const ::testing::UnitTest &, int) override {}

  // Fired after all test activities have ended.
  void OnTestProgramEnd(const ::testing::UnitTest &) override {}

 private:
  void setFollyLog(std::string file) {
    for (auto &c : file) {
      if (c == '/') {
        c = '.';
      }
    }
    auto logPath = fmt::format("{}/{}.txt", FLAGS_log_to_directory, file);
    auto eventPath = fmt::format("{}/{}.events", FLAGS_log_to_directory, file);
    auto config = folly::parseJson(R"JSON({
      "categories": {
        ".": { "level": "INFO", "handlers": ["stderr", "file"] },
        "eventlog": { "level": "INFO", "inherit": false, "propagate": "CRITICAL", "handlers": ["event"] }
      },
      "handlers": {
        "stderr": {
          "type": "stream",
          "options": { "stream": "stderr", "async": "false", "level": "FATAL" }
        },
        "file": {
          "type": "file",
          "options": { "path": "", "async": "true" }
        },
        "event": {
          "type": "event", 
          "options": { "path": "", "async": "true" }
        }
      }
    })JSON");
    config["handlers"]["file"]["options"]["path"] = logPath;
    config["handlers"]["event"]["options"]["path"] = eventPath;
    auto &logger = folly::LoggerDB::get();
    fmt::print("Log to path {}\n", logPath);
    logger.updateConfig(folly::parseLogConfig(folly::toJson(config)));
  }

  void clearEmptyLog(std::string file) {
    for (auto &c : file) {
      if (c == '/') {
        c = '.';
      }
    }
    auto eventPath = fmt::format("{}/{}.events", FLAGS_log_to_directory, file);
    struct stat st;
    auto ret = stat(eventPath.c_str(), &st);
    if (ret == 0 && st.st_size == 0) {
      unlink(eventPath.c_str());
    }
  }

  int iteration_;
};

#undef PLOGF

int main(int argc, char *argv[]) {
  hf3fs::logging::initLogHandlers();
  std::string config = R"JSON({
    "categories": {
      ".": { "level": "ERR", "handlers": ["stderr"] },
      "eventlog": { "level": "INFO", "inherit": false, "propagate": "CRITICAL", "handlers": ["event"] },
    },
    "handlers": {
      "stderr": {
        "type": "stream",
        "options": { "stream": "stderr", "async": "false" }
      },
      "event": {
        "type": "event", 
        "options": { "path": "./events.log", "async": "true" }
      }
    }
  })JSON";
  google::SetCommandLineOptionWithMode("logging", config.c_str(), google::SET_FLAGS_DEFAULT);
  ::testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv);
  hf3fs::logging::initOrDie(FLAGS_logging);
  if (!FLAGS_log_to_directory.empty()) {
    ::testing::UnitTest::GetInstance()->listeners().Append(new TestLoggerSetter);
  }
  hf3fs::SysResource::increaseProcessFDLimit(524288);
  hf3fs::monitor::Monitor::Config monitorConfig;
  monitorConfig.reporters(0).set_type("log");
  if (FLAGS_monitor) {
    hf3fs::monitor::Monitor::start(monitorConfig);
  }

  std::optional<hf3fs::Duration> timeout;
  if (!FLAGS_timeout.empty()) {
    auto res = hf3fs::Duration::from(FLAGS_timeout);
    XLOGF_IF(FATAL, !res, "Init timeout failed: {}", res.error());
    timeout = *res;
  }

  folly::LifoSem timeoutThreadSem;
  std::thread timeoutThread([&] {
    if (timeout && !timeoutThreadSem.try_wait_for(*timeout)) {
      auto pid = hf3fs::SysResource::pid();
      auto command = fmt::format("sudo gdb -batch -ex \"thread apply all bt\" -p {}", pid);
      fmt::print(stderr, "Test timeout after {}. start to execute {}\n", *timeout, command);
      system(command.c_str());
      abort();
    }
  });

  int exitCode = RUN_ALL_TESTS();

  timeoutThreadSem.post();
  timeoutThread.join();

  hf3fs::net::IBManager::stop();
  hf3fs::memory::shutdown();
  if (FLAGS_monitor) {
    hf3fs::monitor::Monitor::stop();
  }
  return exitCode;
}
