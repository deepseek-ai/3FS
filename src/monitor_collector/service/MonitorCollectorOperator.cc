#include "monitor_collector/service/MonitorCollectorOperator.h"

#include <algorithm>

namespace hf3fs::monitor {

static CountRecorder numQueueingSamples{"monitor_collector.num_queueing_samples"};

MonitorCollectorOperator::MonitorCollectorOperator(const MonitorCollectorService::Config &cfg)
    : cfg_(cfg) {
  sampleQueue_ = folly::MPMCQueue<std::vector<Sample>>(cfg.queue_capacity());

  for (int i = 0; i < cfg.conn_threads(); i++) {
    threads_.emplace_back(std::bind_front(&MonitorCollectorOperator::connThreadFunc, this));
  }
  threads_.emplace_back(std::bind_front(&MonitorCollectorOperator::monitorThreadFunc, this));
}

MonitorCollectorOperator::~MonitorCollectorOperator() {
  for (auto &thread : threads_) {
    thread.request_stop();
  }
  for (auto &thread : threads_) {
    thread.join();
  }
}

CoTryTask<void> MonitorCollectorOperator::write(std::vector<Sample> &&samples) {
  sampleQueue_.blockingWrite(std::move(samples));
  numQueueingSamples.addSample(1);
  cv_.notify_one();
  co_return Void();
}

void MonitorCollectorOperator::connThreadFunc(std::stop_token stoken) {
  std::unique_ptr<Reporter> reporter;
  auto &reporterConfig = cfg_.reporter();
  if (reporterConfig.type() == "clickhouse") {
    reporter = std::make_unique<ClickHouseClient>(reporterConfig.clickhouse());
  } else if (reporterConfig.type() == "log") {
    reporter = std::make_unique<LogReporter>(reporterConfig.log());
  } else if (reporterConfig.type() == "monitor_collector") {
    reporter = std::make_unique<MonitorCollectorClient>(reporterConfig.monitor_collector());
  } else {
    XLOGF(FATAL, "Invalid reporter type: {}", reporterConfig.type());
  }

  auto result = reporter->init();
  XLOGF_IF(FATAL, result.hasError(), "Initializing reporter failed. {}", result.error().describe());

  std::vector<Sample> samples;

  while (!stoken.stop_requested()) {
    std::unique_lock lk(m_);
    bool has_data = cv_.wait(lk, stoken, [this, &samples]() { return sampleQueue_.read(samples); });

    if (has_data) {
      std::vector<Sample> gather;
      for (int i = 1; i < cfg_.batch_commit_size(); i++) {
        bool continue_gather = sampleQueue_.read(gather);
        if (!continue_gather) {
          break;
        } else {
          samples.insert(samples.end(), gather.begin(), gather.end());
        }
        numQueueingSamples.addSample(-1);
      }

      samples.erase(std::remove_if(samples.begin(),
                                   samples.end(),
                                   [&cfg = this->cfg_](const Sample &s) {
                                     return cfg.blacklisted_metric_names().count(s.name) > 0;
                                   }),
                    samples.end());

      try {
        reporter->commit(samples);
      } catch (error_t e) {
        XLOGF(ERR, "[ERROR]: client commit failed!");
        XLOGF(ERR, "[ERROR]: {}", e);
      }
    }
  }

  XLOG(INFO, "Database connection thread shutting down.");
}

void MonitorCollectorOperator::monitorThreadFunc(std::stop_token stoken) {
  using namespace std::chrono_literals;
  while (!stoken.stop_requested()) {
    XLOGF(INFO, "Sample queue capacity: {} / {}.", sampleQueue_.size(), cfg_.queue_capacity());
    std::this_thread::sleep_for(5s);
  }
}

}  // namespace hf3fs::monitor
