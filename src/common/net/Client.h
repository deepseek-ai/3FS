#pragma once

#include <folly/concurrency/AtomicSharedPtr.h>
#include <optional>

#include "common/net/IOWorker.h"
#include "common/net/Processor.h"
#include "common/net/RDMAControl.h"
#include "common/net/ThreadPoolGroup.h"
#include "common/serde/ClientContext.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Result.h"

namespace hf3fs::net {

class Client {
 public:
  struct Config : public ConfigBase<Config> {
    CONFIG_OBJ(thread_pool, ThreadPoolGroup::Config);
    CONFIG_OBJ(processor, Processor::Config);
    CONFIG_OBJ(io_worker, IOWorker::Config);
    CONFIG_OBJ(rdma_control, RDMAControlImpl::Config);
    CONFIG_HOT_UPDATED_ITEM(default_timeout, kClientRequestDefaultTimeout);
    CONFIG_HOT_UPDATED_ITEM(default_log_long_running_threshold, kClientRequestLogLongRunningThreshold);
    CONFIG_HOT_UPDATED_ITEM(default_send_retry_times, kDefaultMaxRetryTimes, ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(default_compression_level, 0u);
    CONFIG_HOT_UPDATED_ITEM(default_compression_threshold, 128_KB);
    CONFIG_HOT_UPDATED_ITEM(enable_rdma_control, false);
    CONFIG_HOT_UPDATED_ITEM(force_use_tcp, false);
    CONFIG_HOT_UPDATED_ITEM(default_report_metrics, false);
  };

  explicit Client(const Config &config, const std::string &name = "Cli")
      : config_(config),
        tpg_(name, config_.thread_pool()),
        processor_(serdeServices_, tpg_.procThreadPool(), config_.processor()),
        ioWorker_(processor_, tpg_.ioThreadPool(), tpg_.connThreadPool(), config_.io_worker()),
        clientConfigGuard_(config_.addCallbackGuard([&] { updateDefaultOptions(); })) {
    updateDefaultOptions();
  }
  ~Client() { stopAndJoin(); }

  Result<Void> start(const std::string &name = "Cli") {
    RETURN_AND_LOG_ON_ERROR(processor_.start(name));
    RETURN_AND_LOG_ON_ERROR(serdeServices_.addService(std::make_unique<RDMAControlImpl>(config_.rdma_control()), true));
    return ioWorker_.start(name);
  }

  void stopAndJoin() {
    processor_.stopAndJoin();
    ioWorker_.stopAndJoin();
    tpg_.stopAndJoin();
  }

  serde::ClientContext serdeCtx(Address serverAddr) {
    return serde::ClientContext(ioWorker_, config_.force_use_tcp() ? serverAddr.tcp() : serverAddr, options_);
  }

  auto &tpg() { return tpg_; }

  auto options() const { return options_.load(); }

  void dropConnections(Address addr) { ioWorker_.dropConnections(addr); }

  void checkConnections(Address addr, Duration expiredTime) { ioWorker_.checkConnections(addr, expiredTime); }

 protected:
  void updateDefaultOptions() {
    auto options = std::make_shared<CoreRequestOptions>();
    options->timeout = config_.default_timeout();
    options->logLongRunningThreshold = config_.default_log_long_running_threshold();
    options->sendRetryTimes = config_.default_send_retry_times();
    options->compression = {config_.default_compression_level(), config_.default_compression_threshold()};
    options->enableRDMAControl = config_.enable_rdma_control();
    options->reportMetrics = config_.default_report_metrics();
    options_ = std::move(options);
  }

 private:
  serde::Services serdeServices_;
  const Config &config_;

  ThreadPoolGroup tpg_;
  Processor processor_;
  IOWorker ioWorker_;
  std::unique_ptr<ConfigCallbackGuard> clientConfigGuard_;
  folly::atomic_shared_ptr<const CoreRequestOptions> options_{std::make_shared<CoreRequestOptions>()};
};

}  // namespace hf3fs::net
