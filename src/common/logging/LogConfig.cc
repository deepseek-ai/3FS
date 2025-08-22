#include "LogConfig.h"

#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <folly/dynamic.h>
#include <folly/json.h>
#include <folly/logging/xlog.h>
#include <utility>
#include <vector>

#include "common/utils/MagicEnum.hpp"

namespace hf3fs::logging {
namespace {

String toLower(std::string_view s) {
  String out;
  std::transform(s.begin(), s.end(), std::back_inserter(out), ::tolower);
  return out;
}
struct LogConfigObject {
  static folly::dynamic create(const LogConfig &c, const std::string serverName) {
    folly::dynamic categories = folly::dynamic::object();
    folly::dynamic handlers = folly::dynamic::object();
    for (const auto &cat : c.categories()) {
      addCategory(categories, cat);
    }
    for (const auto &h : c.handlers()) {
      addHandler(handlers, h, serverName);
    }
    return folly::dynamic::object()("categories", categories)("handlers", handlers);
  }

  static void addCategory(folly::dynamic &categories, const LogCategoryConfig &c) {
    for (auto key : c.categories()) {
      folly::dynamic value = folly::dynamic::object();
      value.insert("level", c.level().toString());
      value.insert("inherit", c.inherit());
      value.insert("propagate", c.propagate().toString());
      value.insert("handlers", folly::dynamic::array());
      for (auto &handler : c.handlers()) {
        value["handlers"].push_back(handler);
      }
      categories.insert(key, value);
    }
  }

  static void addHandler(folly::dynamic &handlers, const LogHandlerConfig &c, const std::string serverName) {
    std::string key = c.name();
    folly::dynamic value = folly::dynamic::object()                //
        ("type", toLower(magic_enum::enum_name(c.writer_type())))  // type
        ("options", folly::dynamic::object());                     // options
    auto &options = value["options"];
    value.insert("type", toLower(magic_enum::enum_name(c.writer_type())));
    if (c.start_level() != folly::LogLevel::MIN_LEVEL) {
      options.insert("level", c.start_level().toString());
    }
    switch (c.writer_type()) {
      case LogHandlerConfig::WriterType::STREAM:
        options.insert("stream", toLower(magic_enum::enum_name(c.stream_type())));
        break;
      case LogHandlerConfig::WriterType::FILE:
      case LogHandlerConfig::WriterType::EVENT:
        if (c.file_path().empty()) {
          if (c.name() == "normal") {
            options.insert("path", fmt::format("{}.log", serverName));
          } else {
            options.insert("path", fmt::format("{}.{}.log", serverName, c.name()));
          }
        } else {
          options.insert("path", c.file_path());
        }
        options.insert("async", c.async() ? "true" : "false");
        options.insert("rotate", c.rotate() ? "true" : "false");
        if (c.rotate()) {
          options.insert("max_files", fmt::format("{}", c.max_files()));
          options.insert("max_file_size", fmt::format("{}", c.max_file_size().toInt()));
          options.insert("rotate_on_open", c.rotate_on_open() ? "true" : "false");
        }
    }
    handlers.insert(key, value);
  }
};
}  // namespace

String generateLogConfig(const LogConfig &c, String serverName) {
  std::string json;
  try {
    auto cfg = LogConfigObject::create(c, serverName);
    folly::json::serialization_opts opts;
    opts.pretty_formatting = false;
    json = folly::json::serialize(cfg, opts);
    opts.pretty_formatting = true;
    XLOGF(INFO, "Folly log json configure: {}", folly::json::serialize(cfg, opts));
  } catch (folly::json::print_error &err) {
    XLOGF(FATAL, "Failed to generate log config, error {}", err.what());
  }
  return json;
}

}  // namespace hf3fs::logging
