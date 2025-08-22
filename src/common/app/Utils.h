#pragma once

#include "ApplicationBase.h"

namespace hf3fs::app_detail {
Result<Void> parseFlags(int *argc,
                        char ***argv,
                        ApplicationBase::ConfigFlags &appConfigFlags,
                        ApplicationBase::ConfigFlags &launcherConfigFlags,
                        ApplicationBase::ConfigFlags &configFlags);

void initLogging(const logging::LogConfig &cfg, const String &serverName);
void initCommonComponents(const ApplicationBase::Config &cfg, const String &serverName, flat::NodeId nodeId);
void persistConfig(const config::IConfig &cfg);

std::unique_ptr<ConfigCallbackGuard> makeLogConfigUpdateCallback(logging::LogConfig &cfg, String serverName);

std::unique_ptr<ConfigCallbackGuard> makeMemConfigUpdateCallback(memory::MemoryAllocatorConfig &cfg, String hostname);

void loadAppInfo(std::function<Result<flat::AppInfo>()> launcher, flat::AppInfo &baseInfo);

void initConfig(config::IConfig &cfg,
                const std::vector<config::KeyValue> &updates,
                const flat::AppInfo &appInfo,
                std::function<Result<std::pair<String, String>>()> launcher);

void initConfigFromFile(config::IConfig &cfg,
                        const String &filePath,
                        bool dump,
                        const std::vector<config::KeyValue> &updates);

}  // namespace hf3fs::app_detail
