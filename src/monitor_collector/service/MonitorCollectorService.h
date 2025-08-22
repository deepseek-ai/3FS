#pragma once

#include <condition_variable>
#include <folly/MPMCQueue.h>
#include <mutex>
#include <thread>
#include <vector>

#include "common/monitor/Monitor.h"
#include "common/serde/Service.h"
#include "common/utils/ConfigBase.h"
#include "fbs/monitor_collector/MonitorCollectorService.h"

namespace hf3fs::monitor {
class MonitorCollectorOperator;
class MonitorCollectorService : public serde::ServiceWrapper<MonitorCollectorService, MonitorCollector> {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_OBJ(reporter, hf3fs::monitor::Monitor::ReporterConfig, [](hf3fs::monitor::Monitor::ReporterConfig &c) {
      c.set_type("clickhouse");
    });
    CONFIG_ITEM(conn_threads, 32);
    CONFIG_ITEM(queue_capacity, 204800);
    CONFIG_ITEM(batch_commit_size, 4096);
    CONFIG_ITEM(blacklisted_metric_names, std::set<std::string>{});
  };

  MonitorCollectorService(MonitorCollectorOperator &monitorCollectorOperator)
      : monitorCollectorOperator_(monitorCollectorOperator) {}

  CoTryTask<MonitorCollectorRsp> write(serde::CallContext &ctx, std::vector<Sample> &samples);

 private:
  MonitorCollectorOperator &monitorCollectorOperator_;
};

}  // namespace hf3fs::monitor
