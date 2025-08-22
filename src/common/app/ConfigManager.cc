#include "ConfigManager.h"

#include "common/utils/ConfigBase.h"
#include "common/utils/RenderConfig.h"
#include "common/utils/StringUtils.h"

namespace hf3fs::app {

#define RETURN_AND_RECORD_CONFIG_UPDATE(ret, desc)                                        \
  Result<Void> _r = (ret);                                                                \
  updateConfigRecord(_r.hasError() ? _r.error() : Status(StatusCode::kOK), String(desc)); \
  return _r;

void ConfigManager::updateConfigRecord(Status status, String desc) {
  ConfigUpdateRecord rec;
  rec.updateTime = UtcClock::now();
  rec.result = std::move(status);
  rec.description = std::move(desc);
  lastConfigUpdateRecord = std::move(rec);
}

void ConfigManager::updateConfigContent(config::IConfig &cfg,
                                        const flat::AppInfo *appInfo,
                                        std::string_view configContent,
                                        bool checkDiff) {
  if (!configContent.empty()) {
    lastConfigContent = configContent;
  }
  auto oldConfigStatus = configStatus;
  SCOPE_EXIT {
    XLOGF_IF(INFO,
             configStatus != oldConfigStatus,
             "Update configStatus {} -> {}",
             toStringView(oldConfigStatus),
             toStringView(configStatus));
  };
  auto renderRes = hf3fs::renderConfig(lastConfigContent, appInfo);
  if (renderRes.hasError()) {
    configStatus = ConfigStatus::FAILED;
    return;
  }
  auto expected = cfg.defaultPtr();
  auto updateRes = expected->atomicallyUpdate(std::string_view(*renderRes), /*isHotUpdate=*/false);
  if (updateRes.hasError()) {
    configStatus = ConfigStatus::FAILED;
    return;
  }

  if (checkDiff) {
    config::IConfig::ItemDiff diffs[10];
    auto diffCnt = cfg.diffWith(*expected, std::span(diffs));
    if (diffCnt != 0) {
      String msg = "Config has diffs, display at most 10:";
      for (size_t i = 0; i < diffCnt; ++i) {
        msg += fmt::format("\n{}: actual {}. expected {}", diffs[i].key, diffs[i].left, diffs[i].right);
      }
      XLOG(WARN, msg);
      configStatus = ConfigStatus::DIRTY;
    } else {
      configStatus = ConfigStatus::NORMAL;
    }
  } else {
    configStatus = ConfigStatus::NORMAL;
  }
}

void ConfigManager::updateConfigStatus(config::IConfig &cfg, const flat::AppInfo *appInfo) {
  updateConfigContent(cfg, appInfo, {});
}

Result<Void> ConfigManager::updateConfig(const String &configContent,
                                         std::string_view configDesc,
                                         config::IConfig &cfg,
                                         const flat::AppInfo *appInfo,
                                         bool hotUpdate,
                                         bool checkDiff) {
  auto renderRes = hf3fs::renderConfig(configContent, appInfo);
  if (renderRes.hasError()) {
    XLOGF(ERR, "Update config failed at rendering: {}. desc: {}", renderRes.error(), configDesc);
    RETURN_AND_RECORD_CONFIG_UPDATE(makeError(renderRes.error()), configDesc);
  }
  auto updateRes = cfg.atomicallyUpdate(std::string_view(*renderRes), hotUpdate);
  if (updateRes.hasError()) {
    XLOGF(WARN, "Update config failed at updating: {}. desc: {}", updateRes.error(), configDesc);
    RETURN_AND_RECORD_CONFIG_UPDATE(makeError(updateRes.error()), configDesc);
  }
  XLOGF(INFO, "Update config succeeded. desc: {}\nFull Config:\n{}", configDesc, cfg.toString());
  updateConfigContent(cfg, appInfo, configContent, checkDiff);
  RETURN_AND_RECORD_CONFIG_UPDATE(Void{}, configDesc);
}

Result<Void> ConfigManager::hotUpdateConfig(const String &update,
                                            bool render,
                                            config::IConfig &cfg,
                                            const flat::AppInfo *appInfo) {
  String updateStr;
  if (render) {
    auto renderRes = hf3fs::renderConfig(update, appInfo);
    RETURN_ON_ERROR(renderRes);
    updateStr = *renderRes;
  } else {
    updateStr = update;
  }
  auto res = cfg.atomicallyUpdate(std::string_view{updateStr});
  if (res.hasError()) {
    XLOGF(WARN, "Hot update config failed: {}. Config content:\n{}", res.error(), update);
    RETURN_ERROR(res);
  }
  XLOGF(INFO, "Hot update config succeeded. Config content:\n{}", update);
  updateConfigStatus(cfg, appInfo);
  return Void{};
}
}  // namespace hf3fs::app
