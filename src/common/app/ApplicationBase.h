#pragma once

#include "common/app/AppInfo.h"
#include "common/app/ConfigStatus.h"
#include "common/app/ConfigUpdateRecord.h"
#include "common/logging/LogConfig.h"
#include "common/monitor/Monitor.h"
#include "common/net/Server.h"
#include "common/serde/Serde.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/UtcTimeSerde.h"
#include "memory/common/MemoryAllocatorConfig.h"

namespace hf3fs {
class ApplicationBase {
 public:
  struct Config : public ConfigBase<Config> {
    CONFIG_OBJ(log, logging::LogConfig);
    CONFIG_OBJ(monitor, monitor::Monitor::Config);
    CONFIG_OBJ(memory, memory::MemoryAllocatorConfig);
  };
  int run(int argc, char *argv[]);

  static void handleSignal(int signum);

  using ConfigFlags = std::vector<config::KeyValue>;
  static Result<Void> parseFlags(std::string_view prefix, int *argc, char ***argv, ConfigFlags &flags);

  static Result<Void> initConfig(config::IConfig &cfg,
                                 const String &path,
                                 bool dump,
                                 const std::vector<config::KeyValue> &flags);

  static Result<Void> loadConfig(config::IConfig &cfg, const String &path, const std::vector<config::KeyValue> &flags);

  static Result<Void> updateConfig(const String &configContent, const String &configDesc);

  static Result<Void> hotUpdateConfig(const String &update, bool render);

  static Result<Void> validateConfig(const String &configContent, const String &configDesc);

  static Result<String> getConfigString(std::string_view configKey);

  static Result<std::pair<String, Result<String>>> renderConfig(const String &configContent,
                                                                bool testUpdate,
                                                                bool isHotUpdate);

  static std::optional<flat::AppInfo> getAppInfo();

  static std::optional<app::ConfigUpdateRecord> getLastConfigUpdateRecord();

  static ConfigStatus getConfigStatus();

 protected:
  ApplicationBase();
  ~ApplicationBase() = default;

  virtual void stop() = 0;

  virtual int mainLoop();

  virtual Result<Void> parseFlags(int *argc, char ***argv) = 0;

  virtual Result<Void> initApplication() = 0;

  virtual config::IConfig *getConfig() = 0;

  virtual const flat::AppInfo *info() const { return nullptr; }

  virtual bool configPushable() const { return true; }

  virtual void onConfigUpdated() {}
};

void stopAndJoin(net::Server *server);
}  // namespace hf3fs
