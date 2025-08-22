#pragma once

#include <folly/logging/LogCategoryConfig.h>
#include <folly/logging/LogLevel.h>
#include <vector>

#include "common/utils/ConfigBase.h"
#include "common/utils/Size.h"
#include "common/utils/String.h"

namespace hf3fs::logging {
struct LogLevel {
  LogLevel() = default;
  LogLevel(folly::LogLevel l)
      : level(l) {}
  LogLevel(std::string_view s) { level = folly::stringToLogLevel(folly::StringPiece(s)); }

  String toString() const { return folly::logLevelToString(level); }
  operator folly::LogLevel() const { return level; }

  folly::LogLevel level = folly::LogLevel::INFO;
};

struct LogCategoryConfig : public ConfigBase<LogCategoryConfig> {
  CONFIG_ITEM(categories, std::vector<String>{"."});
  CONFIG_ITEM(level, LogLevel(folly::LogLevel::INFO));
  CONFIG_ITEM(inherit, true);
  CONFIG_ITEM(propagate, LogLevel(folly::LogLevel::MIN_LEVEL));
  CONFIG_ITEM(handlers, std::vector<String>());

 public:
  bool operator==(const LogCategoryConfig &o) const {
    return categories() == o.categories() && level() == o.level() && inherit() == o.inherit() &&
           propagate() == o.propagate() && handlers() == o.handlers();
  }

  bool operator!=(const LogCategoryConfig &o) const { return !(*this == o); }
};

struct LogHandlerConfig : public ConfigBase<LogHandlerConfig> {
  enum class WriterType {
    FILE,
    STREAM,
    EVENT,
  };
  enum class StreamType {
    STDOUT,
    STDERR,
  };
  CONFIG_ITEM(name, "");
  CONFIG_ITEM(writer_type, WriterType::FILE);
  // FILE options
  CONFIG_ITEM(file_path, "");  // leave empty if you want `setupLogConfig` to auto generate path
  CONFIG_ITEM(async, true);
  // ROTATE FILE options
  CONFIG_ITEM(rotate, true);
  CONFIG_ITEM(max_files, 100);
  CONFIG_ITEM(max_file_size, 10_MB);
  CONFIG_ITEM(rotate_on_open, false);
  // STREAM options
  CONFIG_ITEM(stream_type, StreamType::STDERR);
  // SHARED options
  CONFIG_ITEM(start_level, LogLevel(folly::LogLevel::MIN_LEVEL));

 public:
  bool operator==(const LogHandlerConfig &o) const {
    return name() == o.name() && writer_type() == o.writer_type() && file_path() == o.file_path() &&
           async() == o.async() && rotate() == o.rotate() && max_files() == o.max_files() &&
           max_file_size() == o.max_file_size() && rotate_on_open() == o.rotate_on_open() &&
           stream_type() == o.stream_type() && start_level() == o.start_level();
  }

  bool operator!=(const LogHandlerConfig &o) const { return !(*this == o); }
};

struct LogConfig : public ConfigBase<LogConfig> {
  static LogCategoryConfig makeRootCategoryConfig() {
    LogCategoryConfig cfg;
    cfg.set_categories({"."});
    cfg.set_level(folly::LogLevel::INFO);
    cfg.set_handlers({"normal", "err", "fatal"});
    return cfg;
  }

  static LogCategoryConfig makeEventCategoryConfig() {
    LogCategoryConfig cfg;
    cfg.set_categories(std::vector<String>{"eventlog"});
    cfg.set_level(folly::LogLevel::INFO);
    cfg.set_propagate(folly::LogLevel::ERR);
    cfg.set_inherit(false);
    cfg.set_handlers({"event"});
    return cfg;
  }

  static LogHandlerConfig makeNormalHandlerConfig() {
    LogHandlerConfig cfg;
    cfg.set_name("normal");
    cfg.set_async(true);
    cfg.set_start_level(folly::LogLevel::MIN_LEVEL);
    return cfg;
  }

  static LogHandlerConfig makeErrHandlerConfig() {
    LogHandlerConfig cfg;
    cfg.set_name("err");
    cfg.set_async(false);
    cfg.set_start_level(folly::LogLevel::ERR);
    return cfg;
  }

  static LogHandlerConfig makeFatalHandlerConfig() {
    LogHandlerConfig cfg;
    cfg.set_name("fatal");
    cfg.set_async(false);
    cfg.set_start_level(folly::LogLevel::FATAL);
    cfg.set_writer_type(LogHandlerConfig::WriterType::STREAM);
    cfg.set_stream_type(LogHandlerConfig::StreamType::STDERR);
    return cfg;
  }

  static LogHandlerConfig makeEventHandlerConfig() {
    LogHandlerConfig cfg;
    cfg.set_name("event");
    cfg.set_writer_type(LogHandlerConfig::WriterType::EVENT);
    cfg.set_async(true);
    cfg.set_start_level(folly::LogLevel::INFO);
    return cfg;
  }

  CONFIG_HOT_UPDATED_ITEM(categories, std::vector<LogCategoryConfig>({makeRootCategoryConfig()}));
  CONFIG_HOT_UPDATED_ITEM(
      handlers,
      std::vector<LogHandlerConfig>({makeNormalHandlerConfig(), makeErrHandlerConfig(), makeFatalHandlerConfig()}));
};

String generateLogConfig(const LogConfig &c, String serverName);
}  // namespace hf3fs::logging
