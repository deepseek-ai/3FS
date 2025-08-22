#pragma once

#include <atomic>
#include <condition_variable>
#include <folly/ProducerConsumerQueue.h>
#include <mutex>
#include <shared_mutex>

#include "common/monitor/ClickHouseClient.h"
#include "common/monitor/LogReporter.h"
#include "common/monitor/MonitorCollectorClient.h"
#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"
#include "common/utils/Address.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/ObjectPool.h"

namespace hf3fs::monitor {

class MonitorInstance;
class Collector {
 public:
  Collector();
  ~Collector();

  void add(const std::string &name, Recorder &var);
  void del(const std::string &name, Recorder &var);
  void collectAll(size_t bucketIndex, std::vector<Sample> &samples, bool cleanInactive);

 private:
  void resize(size_t n);

  struct Impl;
  std::unique_ptr<Impl> impl;

  friend class MonitorInstance;
};

class Monitor {
 public:
  class ReporterConfig : public ConfigBase<ReporterConfig> {
    CONFIG_VARIANT_TYPE("clickhouse");
    CONFIG_OBJ(clickhouse, ClickHouseClient::Config);
    CONFIG_OBJ(log, LogReporter::Config);
    CONFIG_OBJ(monitor_collector, MonitorCollectorClient::Config);
  };

  class Config : public ConfigBase<Config> {
    CONFIG_OBJ_ARRAY(reporters, ReporterConfig, 4, [](auto &) { return 0; });
    CONFIG_ITEM(num_collectors, 1);
    CONFIG_HOT_UPDATED_ITEM(collect_period, 1_s);
  };

  static Result<Void> start(const Config &config, String hostnameExtra = {});
  static void stop();

  static std::unique_ptr<MonitorInstance> createMonitorInstance();
  static MonitorInstance &getDefaultInstance();
};

class MonitorInstance {
 public:
  ~MonitorInstance();
  Result<Void> start(const Monitor::Config &config, String hostnameExtra = {});
  void stop();

  size_t getCollectorCount() const { return collectorContexts_.size(); }

  Collector &getCollector() {
    static Collector collector;
    return collector;
  }

 private:
  static constexpr size_t kMaxNumSampleBatches = 60;
  using SampleBatch = std::vector<Sample>;
  using SampleBatchPool = ObjectPool<SampleBatch, kMaxNumSampleBatches, kMaxNumSampleBatches>;

  struct CollectorContext {
    folly::ProducerConsumerQueue<SampleBatchPool::Ptr> samplesQueue_ =
        folly::ProducerConsumerQueue<SampleBatchPool::Ptr>(kMaxNumSampleBatches);

    size_t bucketIndex = 0;
    std::mutex mutex_;
    std::condition_variable collectorCond_;
    std::condition_variable reporterCond_;
    std::jthread collectThread_;
    std::jthread reportThread_;
  };

  void periodicallyCollect(CollectorContext &context, const Monitor::Config &config);
  void reportSamples(CollectorContext &context, std::vector<std::unique_ptr<Reporter>> reporters);

  std::atomic<bool> stop_ = true;

  std::vector<std::unique_ptr<CollectorContext>> collectorContexts_;
};

}  // namespace hf3fs::monitor
