#pragma once

#include <condition_variable>
#include <folly/MPMCQueue.h>
#include <mutex>
#include <thread>

#include "common/monitor/Monitor.h"
#include "monitor_collector/service/MonitorCollectorService.h"

namespace hf3fs::monitor {

class MonitorCollectorOperator {
 public:
  MonitorCollectorOperator(const MonitorCollectorService::Config &cfg);
  ~MonitorCollectorOperator();

  CoTryTask<void> write(std::vector<Sample> &&samples);

 private:
  void connThreadFunc(std::stop_token stoken);
  void monitorThreadFunc(std::stop_token stoken);

  const MonitorCollectorService::Config &cfg_;
  std::vector<std::jthread> threads_;
  std::mutex m_;
  std::condition_variable_any cv_;
  folly::MPMCQueue<std::vector<Sample>> sampleQueue_;
};

}  // namespace hf3fs::monitor
