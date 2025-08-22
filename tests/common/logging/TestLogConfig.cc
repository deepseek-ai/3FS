#include <folly/json.h>
#include <folly/logging/LogConfig.h>
#include <folly/logging/LogConfigParser.h>
#include <folly/logging/LogLevel.h>
#include <gtest/gtest.h>

#include "common/logging/LogConfig.h"

namespace hf3fs::test {
namespace {
using namespace ::hf3fs::logging;

void checkConfig(std::string config, std::string expected) {
  auto realObj = folly::parseJson(config);
  auto expectedObj = folly::parseJson(expected);
  ASSERT_EQ(realObj["categories"], expectedObj["categories"]);
  ASSERT_EQ(realObj["handlers"], expectedObj["handlers"]);
  ASSERT_NO_THROW(folly::parseLogConfig(config));
}

TEST(TestLogConfig, testDefault) {
  LogConfig config;
  checkConfig(generateLogConfig(config, "x"), R"JSON({
    "categories": {
      ".": { "level": "INFO", "inherit": true, "propagate": "NONE", "handlers": ["normal", "err", "fatal"] }
    },
    "handlers": {
      "normal": {
        "options": { "path": "x.log", "async": "true", "rotate": "true", "max_files": "100", "max_file_size": "10485760", "rotate_on_open": "false" },
        "type": "file"
      },
      "err": {
        "options": { "level": "ERR", "path": "x.err.log", "async": "false", "rotate": "true", "max_files": "100", "max_file_size": "10485760", "rotate_on_open": "false" },
        "type": "file"
      },
      "fatal": {
        "options": { "level": "FATAL", "stream": "stderr"},
        "type": "stream"
      }
    }
  })JSON");
}

TEST(TestLogConfig, testFileWriter) {
  LogHandlerConfig handlerConfig;
  handlerConfig.set_name("normal");
  handlerConfig.set_writer_type(LogHandlerConfig::WriterType::FILE);
  handlerConfig.set_file_path("a.log");
  handlerConfig.set_async(false);
  handlerConfig.set_rotate(false);
  LogConfig config;
  // this case does not care about other handlers
  config.set_handlers({handlerConfig});
  String s = generateLogConfig(config, "x");
  checkConfig(s, R"JSON({
    "categories": {
      ".": { "level": "INFO", "inherit": true, "propagate": "NONE", "handlers": ["normal", "err", "fatal"] }
    },
    "handlers": {
      "normal": {
        "options": { "path": "a.log", "async": "false", "rotate": "false" },
        "type": "file"
      }
    }
  })JSON");

  handlerConfig.set_file_path("");
  config.set_handlers({handlerConfig});
  s = generateLogConfig(config, "x");
  checkConfig(s, R"JSON({
    "categories": {
      ".": { "level": "INFO", "inherit": true, "propagate": "NONE", "handlers": ["normal", "err", "fatal"] }
    },
    "handlers": {
      "normal": {
        "options": { "path": "x.log", "async": "false", "rotate": "false" },
        "type": "file"
      }
    }
  })JSON");

  handlerConfig.set_start_level(folly::LogLevel::INFO);
  config.set_handlers({handlerConfig});
  s = generateLogConfig(config, "x");
  checkConfig(s, R"JSON({
    "categories": {
      ".": { "level": "INFO", "inherit": true, "propagate": "NONE", "handlers": ["normal", "err", "fatal"] }
    },
    "handlers": {
      "normal": {
        "options": { "level": "INFO", "path": "x.log", "async": "false", "rotate": "false" },
        "type": "file"
      }
    }
  })JSON");

  handlerConfig.set_rotate(true);
  handlerConfig.set_max_files(10);
  handlerConfig.set_max_file_size(100);
  handlerConfig.set_rotate_on_open(true);
  config.set_handlers({handlerConfig});
  s = generateLogConfig(config, "x");
  checkConfig(s, R"JSON({
    "categories": {
      ".": { "level": "INFO", "inherit": true, "propagate": "NONE", "handlers": ["normal", "err", "fatal"] }
    },
    "handlers": {
      "normal": {
        "options": {
          "level": "INFO",
          "path": "x.log",
          "async": "false",
          "rotate": "true",
          "max_files": "10",
          "max_file_size": "100",
          "rotate_on_open": "true"
        },
        "type": "file"
      }
    }
  })JSON");
}

TEST(TestLogConfig, testStreamWriter) {
  LogHandlerConfig handlerConfig;
  handlerConfig.set_name("normal");
  handlerConfig.set_writer_type(LogHandlerConfig::WriterType::STREAM);
  handlerConfig.set_stream_type(LogHandlerConfig::StreamType::STDOUT);
  handlerConfig.set_start_level(folly::LogLevel::INFO);
  LogConfig config;
  // this case does not care about other categories
  config.set_handlers({handlerConfig});

  checkConfig(generateLogConfig(config, "x"), R"JSON({
    "categories": {
      ".": { "level": "INFO", "inherit": true, "propagate": "NONE", "handlers": ["normal","err","fatal"] }
    },
    "handlers": {
      "normal": { "options": { "level": "INFO", "stream": "stdout" }, "type": "stream" }
    }
  })JSON");
}

TEST(TestLogConfig, testOtherHandlers) {
  std::vector<LogHandlerConfig> handlers(2);
  handlers[0].set_name("normal");
  handlers[0].set_writer_type(LogHandlerConfig::WriterType::STREAM);
  handlers[0].set_async(true);
  handlers[0].set_start_level(folly::LogLevel::MIN_LEVEL);
  handlers[1].set_name("err");
  handlers[1].set_writer_type(LogHandlerConfig::WriterType::FILE);
  handlers[1].set_rotate(false);
  handlers[1].set_async(false);
  handlers[1].set_start_level(folly::LogLevel::ERR);
  LogConfig config;
  config.set_handlers(handlers);
  checkConfig(generateLogConfig(config, "X"), R"JSON({
    "categories": {
      ".": { "level": "INFO", "inherit": true, "propagate": "NONE", "handlers": ["normal", "err", "fatal"]}
    },
    "handlers": {
      "normal": {
        "options": { "stream": "stderr" },
        "type": "stream"
      },
      "err": {
        "options": { "level": "ERR", "path": "X.err.log", "async": "false", "rotate": "false" },
        "type": "file"
      }
    }
  })JSON");
}

TEST(TestLogConfig, testEventLog) {
  std::vector<LogCategoryConfig> categories(2);
  std::vector<LogHandlerConfig> handlers(2);
  categories[0].set_categories({"."});
  categories[0].set_level(folly::LogLevel::INFO);
  categories[0].set_handlers({"normal", "err", "fatal"});
  categories[1].set_categories({"eventlog"});
  categories[1].set_inherit(false);
  categories[1].set_propagate(folly::LogLevel::CRITICAL);
  categories[1].set_level(folly::LogLevel::INFO);
  categories[1].set_handlers({"event"});
  handlers[0].set_name("normal");
  handlers[0].set_writer_type(LogHandlerConfig::WriterType::STREAM);
  handlers[0].set_start_level(folly::LogLevel::MIN_LEVEL);
  handlers[1].set_name("err");
  handlers[1].set_writer_type(LogHandlerConfig::WriterType::EVENT);
  handlers[1].set_rotate(false);
  handlers[1].set_async(false);
  handlers[1].set_start_level(folly::LogLevel::ERR);
  LogConfig config;
  config.set_categories(categories);
  config.set_handlers(handlers);
  checkConfig(generateLogConfig(config, "X"), R"JSON({
    "categories": {
      ".": { "level": "INFO", "inherit": true, "propagate": "NONE", "handlers": ["normal","err","fatal"] },
      "eventlog": { "level": "INFO", "inherit": false, "propagate": "CRITICAL", "handlers": ["event"] }
    },
    "handlers": {
      "normal": {
        "type": "stream",
        "options": { "stream": "stderr" }
      },
      "err": {
        "type": "event",
        "options": { "level": "ERR", "path": "X.err.log", "async": "false", "rotate": "false" }
      }
    }
  })JSON");
}

}  // namespace
}  // namespace hf3fs::test
