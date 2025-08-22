#include <array>
#include <cerrno>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/FileUtil.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/hash/Checksum.h>
#include <folly/json.h>
#include <folly/logging/LogConfigParser.h>
#include <folly/logging/Logger.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/xlog.h>
#include <fstream>
#include <gtest/gtest.h>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "common/logging/FileHandlerFactory.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Uuid.h"
#include "fbs/meta/Common.h"
#include "meta/event/Event.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::meta::server {

using flat::UserInfo;

static void logRandomEvent(Event::Type type) {
  auto parent = InodeId(folly::Random::rand64());
  auto name = fmt::format("path-{}", Uuid::random());
  auto inode = InodeId(folly::Random::rand64());
  auto user = folly::Random::rand32();
  Event(type).addField("parent", parent).addField("name", name).addField("inode", inode).addField("user", user).log();
}

static void checkEventLog(std::string_view path, size_t expected) {
  auto fd = open(path.data(), O_RDONLY);
  ASSERT_GE(fd, 0) << errno;

  std::string line;
  std::ifstream file(path.data());
  size_t actual = 0;
  while (std::getline(file, line)) {
    ASSERT_NO_THROW(folly::parseJson(line)) << "Failed to parse " << line;
    actual++;
  }
  ASSERT_EQ(actual, expected);
}

TEST(TestEventLogger, Basic) {
  auto config = folly::LoggerDB::get().getConfig();
  SCOPE_EXIT { folly::LoggerDB::get().resetConfig(config); };
  std::string path = "test-event-logger.log";
  unlink(path.c_str());
  folly::LoggerDB::get().resetConfig(folly::parseLogConfig(R"JSON({
    "categories": {
      ".": { "level": "ERR", "handlers": ["stderr"] },
      "eventlog": { "level": "INFO", "inherit": false, "propagate": "CRITICAL", "handlers": ["event"] },
      "eventlog.CloseWrite": { "level": "FATAL", "inherit": false, "propagate": "CRITICAL", "handlers": ["event"] },
    },
    "handlers": {
      "stderr": {
        "type": "stream",
        "options": { "stream": "stderr", "async": "false" }
      },
      "event": {
        "type": "event", 
        "options": { "path": "test-event-logger.log", "async": "true" }
      }
    }
  })JSON"));
  SCOPE_EXIT {
    struct stat st;
    stat(path.c_str(), &st);
    fmt::print("log file {}, st_size {}\n", path, st.st_size);
    unlink(path.c_str());
  };

  size_t numEvents = 1000;
  for (size_t i = 0; i < numEvents; i++) {
    logRandomEvent(Event::Type::Create);
  }
  // shouldn't log here
  for (size_t i = 0; i < 10; i++) {
    logRandomEvent(Event::Type::CloseWrite);
  }

  sleep(1);
  checkEventLog(path, numEvents);
}

}  // namespace hf3fs::meta::server
