#include "common/utils/ConfigBase.h"

#include <boost/filesystem.hpp>
#include <fmt/format.h>
#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <iostream>

#include "common/utils/FileUtils.h"
#include "common/utils/VersionInfo.h"

DEFINE_string(cfg, "", "Path to .toml config file");
DEFINE_bool(dump_cfg, false, "Dump current config to stdout");
DEFINE_bool(dump_default_cfg, false, "Dump default config to stdout");
DEFINE_string(dump_path, "", "Dump path");

namespace hf3fs::config {

Result<std::vector<KeyValue>> parseFlags(std::string_view prefix, int &argc, char *argv[]) {
  std::vector<KeyValue> result;

  int cnt = argc;
  argc = 1;
  for (int i = 1; i < cnt; ++i) {
    // parse key.
    std::string_view key = argv[i];
    if (!key.starts_with(prefix)) {
      argv[argc++] = argv[i];
      continue;
    }
    key.remove_prefix(prefix.size());

    // parse value.
    auto pos = key.find('=');
    std::string_view value;
    if (pos == std::string_view::npos) {
      if (i + 1 < cnt) {
        value = argv[++i];
      } else {
        return makeError(StatusCode::kInvalidArg, fmt::format("No value for config key {}", key));
      }
    } else {
      value = key.substr(pos + 1);
      key = key.substr(0, pos);
    }

    result.emplace_back(key, value);
  }

  return result;
}

Result<Void> IConfig::init(int *argc, char ***argv, bool follyInit /* = true */) {
  constexpr std::string_view configPrefix = "--config.";
  // 1. parse command line flags.
  auto parseFlagsResult = parseFlags(configPrefix, *argc, *argv);
  if (UNLIKELY(!parseFlagsResult)) {
    XLOGF(ERR, "Parse config from command line flags failed: {}", parseFlagsResult.error());
    return makeError(StatusCode::kConfigInvalidValue);
  }
  if (follyInit) {
    folly::init(argc, argv);
  }
  if (FLAGS_dump_default_cfg) {
    if (FLAGS_dump_path.empty()) {
      std::cout << toString() << std::endl;
    } else {
      auto res = storeToFile(FLAGS_dump_path, toString());
      if (res.hasError()) {
        XLOGF(ERR, "Open file {} failed: {}", FLAGS_dump_path, res.error());
      }
    }
    exit(0);
  }

  // 2. load config from toml files.
  if (!FLAGS_cfg.empty()) {
    toml::parse_result parseResult;
    if (UNLIKELY(!boost::filesystem::exists(FLAGS_cfg))) {
      auto msg = fmt::format("Config file {} not found", FLAGS_cfg);
      XLOG(ERR, msg);
      return makeError(StatusCode::kConfigInvalidValue, std::move(msg));
    }
    try {
      parseResult = toml::parse_file(FLAGS_cfg);
    } catch (const toml::parse_error &e) {
      std::stringstream ss;
      ss << e;
      XLOGF(ERR, "Parse config file [{}] failed: {}", FLAGS_cfg, ss.str());
      return makeError(StatusCode::kConfigParseError);
    } catch (std::exception &e) {
      XLOGF(ERR, "Parse config file [{}] failed: {}", FLAGS_cfg, e.what());
      return makeError(StatusCode::kConfigInvalidValue);
    }

    auto updateResult = update(parseResult, /* isHotUpdate = */ false);
    if (UNLIKELY(!updateResult)) {
      XLOGF(ERR, "Load config file [{}] failed: {}", FLAGS_cfg, updateResult.error());
      return makeError(StatusCode::kConfigInvalidValue);
    }
  }

  // 3. load config from command line flags.
  for (auto &[key, value] : parseFlagsResult.value()) {
    auto findResult = find(key);
    if (UNLIKELY(!findResult)) {
      XLOGF(ERR, "Item {} is not found: {}", key, findResult.error());
      return makeError(StatusCode::kConfigInvalidValue);
    }
    auto &item = *findResult.value();
    auto toml = item.isParsedFromString() ? fmt::format(R"(v = """{}""")", value) : fmt::format("v = {}", value);

    toml::parse_result parseResult;
    try {
      parseResult = toml::parse(toml);
    } catch (const toml::parse_error &e) {
      std::stringstream ss;
      ss << e;
      XLOGF(ERR, "Parse toml {} failed: {}", toml, ss.str());
      return makeError(StatusCode::kConfigParseError);
    } catch (std::exception &e) {
      XLOGF(ERR, "Parse toml {} failed: {}", toml, e.what());
      return makeError(StatusCode::kConfigInvalidValue);
    }

    auto updateResult = item.update(*parseResult["v"].node(), /* isHotUpdate = */ false, key);
    if (UNLIKELY(!updateResult)) {
      XLOGF(ERR, "Load config from toml {} failed: {}", toml, updateResult.error());
      return makeError(StatusCode::kConfigInvalidValue);
    }
  }

  // 4. validate config.
  auto validateResult = validate();
  if (UNLIKELY(!validateResult)) {
    if (!FLAGS_dump_cfg) {
      XLOGF(ERR, "Check default result failed: {}", validateResult.error());
      return makeError(StatusCode::kConfigInvalidValue);
    } else {
      XLOGF(WARN, "Check default result failed: {}", validateResult.error());
    }
  }

  if (FLAGS_dump_cfg) {
    if (FLAGS_dump_path.empty()) {
      std::cout << toString() << std::endl;
    } else {
      auto res = storeToFile(FLAGS_dump_path, toString());
      if (res.hasError()) {
        XLOGF(ERR, "Open file {} failed: {}", FLAGS_dump_path, res.error());
      }
    }
    exit(0);
  }
  return Void{};
}

Result<Void> IConfig::update(std::string_view str, bool isHotUpdate) {
  try {
    auto table = toml::parse(str);
    return update(table, isHotUpdate);
  } catch (const toml::parse_error &e) {
    std::stringstream ss;
    ss << e;
    XLOGF(ERR, "Parse config failed: {}", ss.str());
    return makeError(StatusCode::kConfigParseError);
  } catch (std::exception &e) {
    XLOGF(ERR, "Parse config failed: {}", e.what());
    return makeError(StatusCode::kInvalidArg);
  }
}

Result<Void> IConfig::update(const Path &path, bool isHotUpdate) {
  if (UNLIKELY(!boost::filesystem::exists(path))) {
    auto msg = fmt::format("Config file {} not found", path);
    XLOG(DBG, msg);
    return makeError(StatusCode::kConfigInvalidValue, std::move(msg));
  }
  try {
    auto table = toml::parse_file(path.string());
    return update(table, isHotUpdate);
  } catch (std::exception &e) {
    XLOGF(ERR, "Parse config file {} failed: {}", path.string(), e.what());
    return makeError(StatusCode::kInvalidArg);
  }
}

Result<Void> IConfig::update(const std::vector<KeyValue> &updates, bool isHotUpdate) {
  for (auto &[key, value] : updates) {
    auto findResult = find(key);
    if (UNLIKELY(!findResult)) {
      XLOGF(ERR, "Item {} is not found: {}", key, findResult.error());
      return makeError(StatusCode::kConfigInvalidValue);
    }
    auto &item = *findResult.value();
    auto toml = item.isParsedFromString() ? fmt::format(R"(v = """{}""")", value) : fmt::format("v = {}", value);

    toml::parse_result parseResult;
    try {
      parseResult = toml::parse(toml);
    } catch (const toml::parse_error &e) {
      std::stringstream ss;
      ss << e;
      XLOGF(ERR, "Parse toml {} failed: {}", toml, ss.str());
      return makeError(StatusCode::kConfigParseError);
    } catch (std::exception &e) {
      XLOGF(ERR, "Parse toml {} failed: {}", toml, e.what());
      return makeError(StatusCode::kConfigInvalidValue);
    }

    auto updateResult = item.update(*parseResult["v"].node(), isHotUpdate, key);
    if (UNLIKELY(!updateResult)) {
      XLOGF(ERR, "Load config from toml {} failed: {}", toml, updateResult.error());
      return makeError(StatusCode::kConfigInvalidValue);
    }
  }
  return Void{};
}

}  // namespace hf3fs::config
