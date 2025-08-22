#ifdef OVERRIDE_CXX_NEW_DELETE
#undef OVERRIDE_CXX_NEW_DELETE
#endif

#include "common/monitor/Monitor.h"

#include <chrono>
#include <folly/Synchronized.h>
#include <folly/logging/xlog.h>
#include <folly/system/ThreadName.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/monitor/Recorder.h"
#include "common/monitor/Reporter.h"
#include "common/utils/SysResource.h"
#include "memory/common/OverrideCppNewDelete.h"

namespace hf3fs::monitor {
namespace {

class DummyVariable : public Recorder {
 public:
  DummyVariable(const std::string &name)
      : Recorder(name, std::nullopt, Monitor::getDefaultInstance()) {
    registerRecorder();
  }
  ~DummyVariable() override { unregisterRecorder(); }
  void collect(std::vector<Sample> &) override {}
} dummyVariable("dummy");  // keep variables alive until reporter thread stopped.

using RecorderKey = std::pair<std::string, TagSet>;
struct RecorderKeyHash {
  size_t operator()(const RecorderKey &key) const noexcept { return folly::hash::hash_combine(key.first, key.second); }
};

using RecorderMap = robin_hood::unordered_map<RecorderKey, Recorder *, RecorderKeyHash>;
using LockableRecorderMap = folly::Synchronized<RecorderMap, std::mutex>;
}  // namespace

struct Collector::Impl {
  std::shared_mutex resizeMutex_;
  std::vector<std::unique_ptr<LockableRecorderMap>> maps_;
};

Collector::Collector() {
  impl = std::make_unique<Impl>();
  impl->maps_.push_back(std::make_unique<LockableRecorderMap>());
}

Collector::~Collector() = default;

void Collector::add(const std::string &name, Recorder &var) {
  auto key = std::make_pair(name, var.tag_);
  auto hash = RecorderKeyHash()(key);
  auto lock = std::shared_lock(impl->resizeMutex_);
  auto map = impl->maps_[hash % impl->maps_.size()]->lock();
  auto [it, succeed] = map->try_emplace(std::move(key), &var);
  XLOGF_IF(FATAL,
           !succeed,
           "Monitor two variables with same name and same tag: {}. tags: {}",
           name,
           serde::toJsonString(var.tag_.asMap()));
  return;
}

void Collector::del(const std::string &name, Recorder &var) {
  auto key = std::make_pair(name, var.tag_);
  auto hash = RecorderKeyHash()(key);
  auto lock = std::shared_lock(impl->resizeMutex_);
  auto map = impl->maps_[hash % impl->maps_.size()]->lock();
  auto it = map->find(key);
  if (it != map->end()) {
    map->erase(it);
  } else {
    XLOGF(ERR, "Monitor has no variable with name {} and tags {}", name, serde::toJsonString(var.tag_.asMap()));
  }
}

void Collector::collectAll(size_t bucketIndex, std::vector<Sample> &samples, bool cleanInactive) {
  XLOGF_IF(FATAL,
           impl->maps_.size() <= bucketIndex,
           "Monitor bucketIndex out of range. buckets: {}. bucketIndex: {}",
           impl->maps_.size(),
           bucketIndex);
  auto map = impl->maps_[bucketIndex]->lock();
  for (auto &pair : *map) {
    pair.second->collectAndClean(samples, cleanInactive);
  }
}

void Collector::resize(size_t n) {
  XLOGF_IF(FATAL, n == 0 || n > 1024, "Monitor invalid bucket count: {}", n);

  auto lock = std::unique_lock(impl->resizeMutex_);
  if (impl->maps_.size() == n) {
    return;
  }
  std::vector<std::unique_ptr<LockableRecorderMap>> newMaps(n);
  for (size_t i = 0; i < n; ++i) {
    newMaps[i] = std::make_unique<LockableRecorderMap>();
  }

  for (auto &m : impl->maps_) {
    auto oldmap = m->lock();
    for (auto &[key, v] : *oldmap) {
      auto hash = RecorderKeyHash()(key);
      auto newmap = newMaps[hash % newMaps.size()]->lock();
      newmap->try_emplace(key, v);
    }
  }

  impl->maps_.swap(newMaps);
}

void MonitorInstance::periodicallyCollect(CollectorContext &context, const Monitor::Config &config) {
  static constexpr auto kCleanPeriod = std::chrono::seconds(300);

  auto cleanTime = std::chrono::steady_clock::now();
  auto currentTime = std::chrono::steady_clock::now();
  while (!stop_) {
    auto samples = SampleBatchPool::get();

    if (currentTime - cleanTime > kCleanPeriod) {
      getCollector().collectAll(context.bucketIndex, *samples, true);
      cleanTime = std::chrono::steady_clock::now();
    } else {
      getCollector().collectAll(context.bucketIndex, *samples, false);
    }

    if (LIKELY(!samples->empty())) {
      context.samplesQueue_.write(std::move(samples));
      context.reporterCond_.notify_one();
    }

    auto collect_period = config.collect_period();
    auto nextRunTime = currentTime + collect_period;
    currentTime = std::chrono::steady_clock::now();
    if (currentTime <= nextRunTime) {
      auto lock = std::unique_lock(context.mutex_);
      context.collectorCond_.wait_for(lock, nextRunTime - currentTime);
      currentTime = nextRunTime;
    } else {
      XLOGF(ERR,
            "A report task takes more than {} second: {}ms",
            collect_period,
            std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - nextRunTime).count());
    }
  }
}

void MonitorInstance::reportSamples(CollectorContext &context, std::vector<std::unique_ptr<Reporter>> reporters) {
  while (!stop_) {
    SampleBatchPool::Ptr samples;
    if (!context.samplesQueue_.read(samples)) {
      auto lock = std::unique_lock(context.mutex_);
      context.reporterCond_.wait(lock);
      continue;
    }
    for (auto &reporter : reporters) {
      reporter->commit(*samples);
    }
  }
}

Result<Void> Monitor::start(const Config &config, String hostnameExtra) {
  return getDefaultInstance().start(config, std::move(hostnameExtra));
}

void Monitor::stop() { getDefaultInstance().stop(); }

std::unique_ptr<MonitorInstance> Monitor::createMonitorInstance() { return std::make_unique<MonitorInstance>(); }

MonitorInstance &Monitor::getDefaultInstance() {
  static MonitorInstance defaultInstance;
  return defaultInstance;
}

Result<Void> MonitorInstance::start(const Monitor::Config &config, String hostnameExtra) {
  if (!stop_) return Void{};
  stop_ = false;

  getCollector().resize(config.num_collectors());
  for (int i = 0; i < config.num_collectors(); ++i) {
    collectorContexts_.push_back(std::make_unique<CollectorContext>());
    collectorContexts_.back()->bucketIndex = i;
  }

  // 1. set hostname.
  auto hostnameResult = SysResource::hostname(true);
  RETURN_ON_ERROR(hostnameResult);
  auto podnameResult = SysResource::hostname(false);
  RETURN_ON_ERROR(podnameResult);
  if (hostnameExtra.empty()) {
    Recorder::setHostname(hostnameResult.value(), podnameResult.value());
  } else {
    auto append = [&](const String &hostname) { return fmt::format("{}({})", hostname, hostnameExtra); };
    Recorder::setHostname(append(hostnameResult.value()), append(podnameResult.value()));
  }

  for (int i = 0; i < config.num_collectors(); ++i) {
    // 2. create reporter.
    std::vector<std::unique_ptr<Reporter>> reporters;
    for (auto i = 0ul; i < config.reporters_length(); ++i) {
      auto &reporterConfig = config.reporters(i);
      if (reporterConfig.type() == "clickhouse") {
        reporters.push_back(std::make_unique<ClickHouseClient>(reporterConfig.clickhouse()));
      } else if (reporterConfig.type() == "log") {
        reporters.push_back(std::make_unique<LogReporter>(reporterConfig.log()));
      } else if (reporterConfig.type() == "monitor_collector") {
        reporters.push_back(std::make_unique<MonitorCollectorClient>(reporterConfig.monitor_collector()));
      }
      RETURN_ON_ERROR(reporters.back()->init());
    }

    // 3. start collect and report threads.
    collectorContexts_[i]->collectThread_ =
        std::jthread(&MonitorInstance::periodicallyCollect, this, std::ref(*collectorContexts_[i]), std::ref(config));
    folly::setThreadName(collectorContexts_[i]->collectThread_.get_id(), "Collector");
    collectorContexts_[i]->reportThread_ =
        std::jthread(&MonitorInstance::reportSamples, this, std::ref(*collectorContexts_[i]), std::move(reporters));
    folly::setThreadName(collectorContexts_[i]->reportThread_.get_id(), "Reporter");
  }
  return Void{};
}

void MonitorInstance::stop() {
  if (!stop_) {
    stop_ = true;
    for (auto &context : collectorContexts_) {
      context->reporterCond_.notify_one();
      context->collectorCond_.notify_one();
      context->collectThread_ = std::jthread{};
      context->reportThread_ = std::jthread{};
    }
  }
}

MonitorInstance::~MonitorInstance() { stop(); }

void Recorder::registerRecorder() {
  if (!register_) {
    monitor_.getCollector().add(this->name(), *this);
    register_ = true;
  }
}
void Recorder::unregisterRecorder() {
  if (register_) {
    monitor_.getCollector().del(this->name(), *this);
    register_ = false;
  }
}

}  // namespace hf3fs::monitor
