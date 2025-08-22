#include "Utils.h"

#include "common/logging/LogInit.h"
#include "common/utils/FileUtils.h"
#include "common/utils/RenderConfig.h"

DECLARE_string(cfg);
DECLARE_bool(dump_cfg);
DECLARE_bool(dump_default_cfg);
DECLARE_bool(use_local_cfg);
DECLARE_string(cfg_persist_prefix);

extern "C" {
// jemalloc
__attribute__((weak)) void __attribute__((nothrow))
malloc_stats_print(void (*write_cb)(void *, const char *), void *je_cbopaque, const char *opts);
}

namespace hf3fs::app_detail {
namespace {

bool profiling_active = false;

bool je_profiling(bool active, const char *prefix) {
  /*
    prof.active (bool) rw [--enable-prof]
    Control whether sampling is currently active. See the opt.prof_active option for additional information, as well
    as the interrelated thread.prof.active mallctl.

    prof.dump (const char *) -w [--enable-prof]
    Dump a memory profile to the specified file, or if NULL is specified, to a file according to the pattern
    <prefix>.<pid>.<seq>.m<mseq>.heap, where <prefix> is controlled by the opt.prof_prefix and prof.prefix options.

    prof.prefix (const char *) -w [--enable-prof]
    Set the filename prefix for profile dumps. See opt.prof_prefix for the default setting. This can be useful to
    differentiate profile dumps such as from forked processes.
  */

  if (profiling_active) {
    // first dump memory profiling

    if (prefix != nullptr) {
      auto err = mallctl("prof.prefix", nullptr, nullptr, &prefix, sizeof(prefix));

      if (err) {
        fprintf(stderr, "Failed to set profiling dump prefix: %s, error: %d\n", prefix, err);
        return false;
      }
    }

    auto err = mallctl("prof.dump", nullptr, nullptr, nullptr, 0);

    if (err) {
      fprintf(stderr, "Failed to dump memory profiling, prefix: %s, error: %d\n", prefix, err);
      return false;
    }

    fprintf(stderr, "Memory profiling result saved with prefix: %s\n", prefix);
  }

  if (profiling_active == active) {
    fprintf(stderr, "Memory profiling already %s\n", active ? "enabled" : "disabled");
    return true;
  }

  auto err = mallctl("prof.active", nullptr, nullptr, (void *)&active, sizeof(active));

  if (err) {
    fprintf(stderr, "Failed to %s memory profiling, error :%d\n", active ? "enable" : "disable", err);
    return false;
  }

  fprintf(stderr,
          "Memory profiling set from '%s' to '%s'\n",
          profiling_active ? "active" : "inactive",
          active ? "active" : "inactive");
  profiling_active = active;

  return true;
}

void je_logstatus(char *buf, size_t size) {
  size_t allocated, active, metadata, resident, mapped, retained, epoch = 1;
  size_t len = sizeof(size_t);

  /*
    stats.allocated (size_t) r- [--enable-stats]
    Total number of bytes allocated by the application.

    stats.active (size_t) r- [--enable-stats]
    Total number of bytes in active pages allocated by the application. This is a multiple of the page size, and
    greater than or equal to stats.allocated. This does not include stats.arenas.<i>.pdirty, stats.arenas.<i>.pmuzzy,
    nor pages entirely devoted to allocator metadata.

    stats.metadata (size_t) r- [--enable-stats]
    Total number of bytes dedicated to metadata, which comprise base allocations used for bootstrap-sensitive
    allocator metadata structures (see stats.arenas.<i>.base) and internal allocations (see
    stats.arenas.<i>.internal). Transparent huge page (enabled with opt.metadata_thp) usage is not considered.

    stats.resident (size_t) r- [--enable-stats]
    Maximum number of bytes in physically resident data pages mapped by the allocator, comprising all pages dedicated
    to allocator metadata, pages backing active allocations, and unused dirty pages. This is a maximum rather than
    precise because pages may not actually be physically resident if they correspond to demand-zeroed virtual memory
    that has not yet been touched. This is a multiple of the page size, and is larger than stats.active.

    stats.mapped (size_t) r- [--enable-stats]
    Total number of bytes in active extents mapped by the allocator. This is larger than stats.active. This does not
    include inactive extents, even those that contain unused dirty pages, which means that there is no strict ordering
    between this and stats.resident.

    stats.retained (size_t) r- [--enable-stats]
    Total number of bytes in virtual memory mappings that were retained rather than being returned to the operating
    system via e.g. munmap(2) or similar. Retained virtual memory is typically untouched, decommitted, or purged, so
    it has no strongly associated physical memory (see extent hooks for details). Retained memory is excluded from
    mapped memory statistics, e.g. stats.mapped.
  */

  if (mallctl("epoch", nullptr, nullptr, &epoch, sizeof(epoch)) == 0 &&
      mallctl("stats.allocated", &allocated, &len, nullptr, 0) == 0 &&
      mallctl("stats.active", &active, &len, nullptr, 0) == 0 &&
      mallctl("stats.metadata", &metadata, &len, nullptr, 0) == 0 &&
      mallctl("stats.resident", &resident, &len, nullptr, 0) == 0 &&
      mallctl("stats.mapped", &mapped, &len, nullptr, 0) == 0 &&
      mallctl("stats.retained", &retained, &len, nullptr, 0) == 0) {
    // Get the jemalloc config string from environment variable (for logging purposes only).
    const char *configStr = std::getenv("JE_MALLOC_CONF");
    std::snprintf(buf,
                  size,
                  "jemalloc enabled (JE_MALLOC_CONF=\"%s\", profiling=%s), "
                  "current allocated=%zu active=%zu metadata=%zu resident=%zu mapped=%zu, retained=%zu",
                  configStr == nullptr ? "(null)" : configStr,
                  profiling_active ? "active" : "inactive",
                  allocated,
                  active,
                  metadata,
                  resident,
                  mapped,
                  retained);
  }
}

}  // namespace

Result<Void> parseFlags(int *argc,
                        char ***argv,
                        ApplicationBase::ConfigFlags &appConfigFlags,
                        ApplicationBase::ConfigFlags &launcherConfigFlags,
                        ApplicationBase::ConfigFlags &configFlags) {
  static constexpr std::string_view appConfigPrefix = "--app_config.";
  static constexpr std::string_view launcherConfigPrefix = "--launcher_config.";
  static constexpr std::string_view dynamicConfigPrefix = "--config.";

  RETURN_ON_ERROR(ApplicationBase::parseFlags(appConfigPrefix, argc, argv, appConfigFlags));
  RETURN_ON_ERROR(ApplicationBase::parseFlags(launcherConfigPrefix, argc, argv, launcherConfigFlags));
  RETURN_ON_ERROR(ApplicationBase::parseFlags(dynamicConfigPrefix, argc, argv, configFlags));
  return Void{};
}

void initLogging(const logging::LogConfig &cfg, const String &serverName) {
  auto logConfigStr = logging::generateLogConfig(cfg, serverName);
  XLOGF(INFO, "LogConfig: {}", logConfigStr);
  logging::initOrDie(logConfigStr);
}

void initCommonComponents(const ApplicationBase::Config &cfg, const String &serverName, flat::NodeId nodeId) {
  initLogging(cfg.log(), serverName);

  XLOGF(INFO, "{}", VersionInfo::full());

  XLOGF(INFO, "Init waiter singleton {}", fmt::ptr(&net::Waiter::instance()));

  auto monitorResult =
      monitor::Monitor::start(cfg.monitor(), nodeId == 0 ? "" : fmt::format("Node_{}", nodeId.toUnderType()));
  XLOGF_IF(FATAL, !monitorResult, "Start monitor failed: {}", monitorResult.error());
}

void persistConfig(const config::IConfig &cfg) {
  if (FLAGS_cfg_persist_prefix.empty()) {
    XLOGF(INFO, "skip config dumping: cfg_persist_prefix is empty");
    return;
  }
  constexpr auto usPerSecond = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::seconds(1)).count();
  auto now = UtcClock::now();
  auto us = now.toMicroseconds() % usPerSecond;
  auto path = fmt::format("{}_{:%Y%m%d_%H%M%S}_{:06d}", FLAGS_cfg_persist_prefix, now, us);
  auto storeRes = storeToFile(path, cfg.toString());
  if (storeRes) {
    XLOGF(INFO, "Dump full config to {}", path);
  } else {
    XLOGF(ERR, "Dump full config to {} failed: {}", path, storeRes.error());
  }
}

std::unique_ptr<ConfigCallbackGuard> makeLogConfigUpdateCallback(logging::LogConfig &cfg, String serverName) {
  return cfg.addCallbackGuard([&cfg, serverName = std::move(serverName)] { initLogging(cfg, serverName); });
}

std::unique_ptr<ConfigCallbackGuard> makeMemConfigUpdateCallback(memory::MemoryAllocatorConfig &cfg, String hostname) {
  if (malloc_stats_print == nullptr) {
    XLOGF(WARNING, "not dynamically linked jemalloc");
  } else {
    XLOGF(WARNING, "dynamically linked jemalloc");
  }
  return cfg.addCallbackGuard([&cfg, hostname = std::move(hostname)] {
    if (malloc_stats_print == nullptr) {
      return;
    }
    // turn on/off memory profiling
    std::string prefix = fmt::format("{}{}", cfg.prof_prefix(), hostname);
    je_profiling(cfg.prof_active(), prefix.c_str());
    // log memory allocator status
    char logbuf[512] = {0};
    je_logstatus(logbuf, sizeof(logbuf));
    XLOG(WARN, logbuf);
  });
}

String loadConfigFromLauncher(std::function<Result<std::pair<String, String>>()> launcher) {
  XLOGF(INFO, "Start load config from launcher");
  auto retryInterval = std::chrono::milliseconds(10);
  constexpr auto maxRetryInterval = std::chrono::milliseconds(1000);
  for (int i = 0; i < 20; ++i) {
    auto loadConfigRes = launcher();
    if (loadConfigRes) {
      const auto &[content, desc] = *loadConfigRes;
      XLOGF(INFO, "Load config from launcher finished {}", desc);
      return content;
    }

    XLOGF(CRITICAL, "Load config by launcher failed: {}\nretryCount: {}", loadConfigRes.error(), i);
    std::this_thread::sleep_for(retryInterval);
    retryInterval = std::min(2 * retryInterval, maxRetryInterval);
  }
  XLOGF(FATAL, "Load config from launcher failed, stop retrying");
  __builtin_unreachable();
}

void loadAppInfo(std::function<Result<flat::AppInfo>()> launcher, flat::AppInfo &baseInfo) {
  XLOGF(INFO, "Start load AppInfo from launcher");
  auto retryInterval = std::chrono::milliseconds(10);
  constexpr auto maxRetryInterval = std::chrono::milliseconds(1000);
  for (int i = 0; i < 20; ++i) {
    auto loadRes = launcher();
    if (loadRes) {
      baseInfo = *loadRes;
      XLOGF(INFO,
            "Load AppInfo from launcher finished. AppInfo: {}\nclusterId: {}\ntags: {}",
            serde::toJsonString(static_cast<const flat::FbsAppInfo &>(baseInfo)),
            baseInfo.clusterId,
            serde::toJsonString(baseInfo.tags));
      return;
    }

    XLOGF(CRITICAL, "Load AppInfo from launcher failed: {}\nretryCount: {}", loadRes.error(), i);
    std::this_thread::sleep_for(retryInterval);
    retryInterval = std::min(2 * retryInterval, maxRetryInterval);
  }
  XLOGF(FATAL, "Load AppInfo from launcher failed, stop retrying");
  __builtin_unreachable();
}

String loadConfigFromFile() {
  XLOGF(INFO, "Use local server config file: {}", FLAGS_cfg);
  auto res = loadFile(FLAGS_cfg);
  XLOGF_IF(FATAL, !res, "Load config from {} failed: {}", FLAGS_cfg, res.error());
  return *res;
}

String loadConfigTemplate(config::IConfig &cfg, std::function<Result<std::pair<String, String>>()> launcher) {
  if (!FLAGS_cfg.empty()) {
    return loadConfigFromFile();
  } else if (FLAGS_use_local_cfg) {
    XLOGF(INFO, "Use default server config");
    return cfg.toString();
  } else {
    return loadConfigFromLauncher(std::move(launcher));
  }
}

void initConfig(config::IConfig &cfg,
                const std::vector<config::KeyValue> &updates,
                const flat::AppInfo &appInfo,
                std::function<Result<std::pair<String, String>>()> launcher) {
  auto configTemplate = loadConfigTemplate(cfg, std::move(launcher));
  XLOGF(INFO, "Full Config Template:\n{}", configTemplate);
  auto renderRes = hf3fs::renderConfig(configTemplate, &appInfo);
  XLOGF_IF(FATAL, !renderRes, "Render config failed: {}\nTemplate: {}", renderRes.error(), configTemplate);

  auto initRes = cfg.atomicallyUpdate(std::string_view(*renderRes), /*isHotUpdate=*/false);
  XLOGF_IF(FATAL, !initRes, "Init config failed: {}", initRes.error());

  auto res = cfg.update(updates, /*isHotUpdate=*/false);
  XLOGF_IF(FATAL, !res, "Update config failed: {}", res.error());

  if (FLAGS_dump_cfg) {
    fmt::print("{}\n", cfg.toString());
    exit(0);
  }
}

void initConfigFromFile(config::IConfig &cfg,
                        const String &filePath,
                        bool dump,
                        const std::vector<config::KeyValue> &updates) {
  if (dump) {
    fmt::print("{}\n", cfg.toString());
    exit(0);
    __builtin_unreachable();
  }
  auto loadRes = loadFile(filePath);
  XLOGF_IF(FATAL, !loadRes, "Load launcher config failed: {}. filePath: {}", loadRes.error(), filePath);
  auto renderRes = hf3fs::renderConfig(*loadRes, nullptr);
  XLOGF_IF(FATAL, !renderRes, "Render launcher config failed: {}. filePath: {}", renderRes.error(), filePath);
  auto initRes = cfg.atomicallyUpdate(std::string_view(*renderRes), /*isHotUpdate=*/false);
  XLOGF_IF(FATAL, !initRes, "Init launcher config failed: {}. filePath: {}", initRes.error(), filePath);
  auto updateRes = cfg.update(updates, /*isHotUpdate=*/false);
  XLOGF_IF(FATAL, !updateRes, "Update launcher config failed: {}. filePath: {}", updateRes.error(), filePath);
}
}  // namespace hf3fs::app_detail
