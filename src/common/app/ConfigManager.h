#pragma once

#include <optional>

#include "ConfigStatus.h"
#include "ConfigUpdateRecord.h"

namespace hf3fs::config {
struct IConfig;
}

namespace hf3fs::flat {
struct AppInfo;
}

namespace hf3fs::app {
struct ConfigManager {
  void updateConfigRecord(Status status, String desc);
  void updateConfigContent(config::IConfig &cfg,
                           const flat::AppInfo *appInfo,
                           std::string_view configContent,
                           bool checkDiff = true);
  void updateConfigStatus(config::IConfig &cfg, const flat::AppInfo *appInfo);

  Result<Void> updateConfig(const String &configContent,
                            std::string_view configDesc,
                            config::IConfig &cfg,
                            const flat::AppInfo *appInfo,
                            bool hotUpdate = true,
                            bool checkDiff = true);

  Result<Void> hotUpdateConfig(const String &update, bool render, config::IConfig &cfg, const flat::AppInfo *appInfo);

  std::optional<ConfigUpdateRecord> lastConfigUpdateRecord;
  String lastConfigContent;
  ConfigStatus configStatus = ConfigStatus::NORMAL;
};
}  // namespace hf3fs::app
