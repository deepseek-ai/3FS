#include "common/app/ApplicationBase.h"

#include <folly/init/Init.h>
#include <iostream>
#include <sys/signal.h>

#include "common/app/ConfigManager.h"
#include "common/app/Thread.h"
#include "common/utils/OptionalUtils.h"
#include "common/utils/RenderConfig.h"
#include "common/utils/StringUtils.h"
#include "memory/common/GlobalMemoryAllocator.h"

DECLARE_bool(release_version);

namespace hf3fs {
namespace {
std::mutex appMutex;
ApplicationBase *globalApp = nullptr;

std::mutex loopMutex;
std::condition_variable loopCv;
std::atomic<bool> exitLoop = false;
std::atomic<int> exitCode = 0;

app::ConfigManager &getConfigManager() {
  static app::ConfigManager cm;
  return cm;
}

#define RETURN_AND_RECORD_CONFIG_UPDATE(ret, desc)                                                     \
  Result<Void> _r = (ret);                                                                             \
  getConfigManager().updateConfigRecord(_r.hasError() ? _r.error() : Status(StatusCode::kOK), (desc)); \
  return _r;

}  // namespace

ApplicationBase::ApplicationBase() { globalApp = this; }

void ApplicationBase::handleSignal(int signum) {
  XLOGF(ERR, "Handle {} signal.", strsignal(signum));
  exitLoop = true;
  if (signum == SIGUSR2) {
    exitCode = 128 + SIGUSR2;
  }
  loopCv.notify_one();
}

int ApplicationBase::run(int argc, char *argv[]) {
  Thread::blockInterruptSignals();

  // 20 parse flags
  auto parseFlagsRes = parseFlags(&argc, &argv);
  XLOGF_IF(FATAL, !parseFlagsRes, "Parse flags failed: {}", parseFlagsRes.error());

  // 30 init folly
  folly::init(&argc, &argv);

  if (FLAGS_release_version) {
    fmt::print("{}\n{}\n", VersionInfo::full(), VersionInfo::commitHashFull());
    return 0;
  }

  // 40 init application
  auto initRes = initApplication();
  XLOGF_IF(FATAL, !initRes, "Init application failed: {}", initRes.error());

  auto exitCode = mainLoop();

  memory::shutdown();
  stop();

  return exitCode;
}

int ApplicationBase::mainLoop() {
  signal(SIGINT, handleSignal);
  signal(SIGTERM, handleSignal);
  signal(SIGUSR1, handleSignal);
  signal(SIGUSR2, handleSignal);

  Thread::unblockInterruptSignals();

  {
    auto lock = std::unique_lock(loopMutex);
    loopCv.wait(lock, [] { return exitLoop.load(); });
  }

  return exitCode.load();
}

Result<Void> ApplicationBase::parseFlags(std::string_view prefix, int *argc, char ***argv, ConfigFlags &flags) {
  auto res = config::parseFlags(prefix, *argc, *argv);
  if (!res) {
    XLOGF(ERR, "Parse config flags start with {} failed: {}", prefix, res.error());
    return makeError(StatusCode::kConfigInvalidValue,
                     fmt::format("Parse config flags start with {} failed: {}", prefix, res.error()));
  }
  flags = std::move(*res);
  return Void{};
}

Result<Void> ApplicationBase::initConfig(config::IConfig &cfg,
                                         const String &path,
                                         bool dump,
                                         const std::vector<config::KeyValue> &flags) {
  if (dump) {
    std::cout << cfg.toString() << std::endl;
    exit(0);
    __builtin_unreachable();
  }
  return loadConfig(cfg, path, flags);
}

Result<Void> ApplicationBase::updateConfig(const String &configContent, const String &configDesc) {
  auto lock = std::unique_lock(appMutex);
  if (!globalApp || !globalApp->getConfig()) {
    XLOGF(WARN, "Update config ignored: no app found. desc: {}", configDesc);
    RETURN_AND_RECORD_CONFIG_UPDATE(makeError(StatusCode::kNoApplication), configDesc);
  }
  if (!globalApp->configPushable()) {
    XLOGF(WARN,
          "Update config ignored: this app declares that its config is not hotupdatable. desc: {}\nConfig content:\n{}",
          configDesc,
          configContent);
    RETURN_AND_RECORD_CONFIG_UPDATE(makeError(StatusCode::kCannotPushConfig), configDesc);
  }
  auto res = getConfigManager().updateConfig(configContent, configDesc, *globalApp->getConfig(), globalApp->info());
  if (res) {
    globalApp->onConfigUpdated();
  }
  return res;
}

Result<std::pair<String, Result<String>>> ApplicationBase::renderConfig(const String &configContent,
                                                                        bool testUpdate,
                                                                        bool isHotUpdate) {
  auto lock = std::unique_lock(appMutex);
  if (!globalApp) {
    return makeError(StatusCode::kNoApplication);
  }
  auto renderRes = hf3fs::renderConfig(configContent, globalApp->info());
  RETURN_ON_ERROR(renderRes);
  if (!testUpdate) {
    return std::make_pair(std::move(*renderRes), Result<String>(""));
  }
  auto *cfg = globalApp->getConfig();
  if (!cfg) {
    return makeError(StatusCode::kNoApplication);
  }
  auto newCfg = cfg->clonePtr();
  auto updateRes = newCfg->atomicallyUpdate(std::string_view(*renderRes), isHotUpdate);
  if (updateRes.hasError()) {
    return std::make_pair(std::move(*renderRes), Result<String>(makeError(updateRes.error())));
  } else {
    return std::make_pair(std::move(*renderRes), Result<String>(newCfg->toString()));
  }
}

Result<Void> ApplicationBase::hotUpdateConfig(const String &update, bool render) {
  auto lock = std::unique_lock(appMutex);
  if (!globalApp || !globalApp->getConfig()) {
    return makeError(StatusCode::kNoApplication);
  }
  auto res = getConfigManager().hotUpdateConfig(update, render, *globalApp->getConfig(), globalApp->info());
  if (res) {
    globalApp->onConfigUpdated();
  }
  return res;
}

Result<Void> ApplicationBase::validateConfig(const String &configContent, const String &configDesc) {
  auto lock = std::unique_lock(appMutex);
  if (!globalApp) {
    XLOGF(WARN, "Validate config ignored: no app found. desc: {}\nConfig content:\n{}", configDesc, configContent);
    return Void{};
  }
  auto *cfg = globalApp->getConfig();
  if (!cfg) {
    XLOGF(WARN, "Validate config ignored: no config found. desc: {}\nConfig content:\n{}", configDesc, configContent);
    return Void{};
  }
  auto renderRes = hf3fs::renderConfig(configContent, globalApp->info());
  if (renderRes.hasError()) {
    XLOGF(ERR,
          "Validate config failed at rendering: {}. desc: {}\nConfig content:\n{}",
          renderRes.error(),
          configDesc,
          configContent);
    RETURN_ERROR(renderRes);
  }
  auto validateRes = cfg->validateUpdate(std::string_view(*renderRes));
  if (validateRes.hasError()) {
    XLOGF(ERR,
          "Validate config failed at validating: {}. desc: {}\nConfig content:\n{}\nRendered content:\n{}",
          validateRes.error(),
          configDesc,
          configContent,
          *renderRes);
    RETURN_ERROR(validateRes);
  }
  XLOGF(INFO,
        "Validate config succeeded. desc: {}\nConfig content:\n{}\nRendered content:\n{}",
        configDesc,
        configContent,
        *renderRes);
  return Void{};
}

Result<Void> ApplicationBase::loadConfig(config::IConfig &cfg,
                                         const String &path,
                                         const std::vector<config::KeyValue> &flags) {
  if (!path.empty()) {
    XLOGF(INFO, "Try to load config from file [{}]", path);

    toml::parse_result parseResult;
    try {
      parseResult = toml::parse_file(path);
    } catch (const toml::parse_error &e) {
      std::stringstream ss;
      ss << e;
      XLOGF(ERR, "Parse config file [{}] failed: {}", path, ss.str());
      return makeError(StatusCode::kConfigParseError);
    } catch (std::exception &e) {
      XLOGF(ERR, "Parse config file [{}] failed: {}", path, e.what());
      return makeError(StatusCode::kConfigInvalidValue);
    }

    auto updateResult = cfg.update(parseResult, /* isHotUpdate = */ false);
    if (UNLIKELY(!updateResult)) {
      XLOGF(ERR, "Update config from file [{}] failed: {}", path, updateResult.error());
      return makeError(StatusCode::kConfigInvalidValue);
    }
  }

  auto updateResult = cfg.update(flags, /* isHotUpdate = */ false);
  if (UNLIKELY(!updateResult)) {
    XLOGF(ERR, "Update config from command-line flags failed: {}", updateResult.error());
    return makeError(StatusCode::kConfigInvalidValue);
  }

  return Void{};
}

std::optional<flat::AppInfo> ApplicationBase::getAppInfo() {
  auto lock = std::unique_lock(appMutex);
  if (globalApp) {
    return makeOptional(globalApp->info());
  }
  return std::nullopt;
}

Result<String> ApplicationBase::getConfigString(std::string_view configKey) {
  auto lock = std::unique_lock(appMutex);
  if (auto *cfg = globalApp ? globalApp->getConfig() : nullptr; cfg) {
    auto toml = cfg->toToml(configKey);
    RETURN_ON_ERROR(toml);
    std::stringstream ss;
    ss << toml::toml_formatter(*toml, toml::toml_formatter::default_flags & ~toml::format_flags::indentation);
    return ss.str();
  }
  return makeError(StatusCode::kNoApplication);
}

std::optional<app::ConfigUpdateRecord> ApplicationBase::getLastConfigUpdateRecord() {
  auto lock = std::unique_lock(appMutex);
  return getConfigManager().lastConfigUpdateRecord;
}

ConfigStatus ApplicationBase::getConfigStatus() {
  auto lock = std::unique_lock(appMutex);
  return getConfigManager().configStatus;
}

void stopAndJoin(net::Server *server) {
  XLOGF(INFO, "Stop the server...");
  if (server) {
    server->stopAndJoin();
  }
  XLOGF(INFO, "Stop server finished.");
  monitor::Monitor::stop();
  hf3fs::net::IBManager::stop();
}
}  // namespace hf3fs
