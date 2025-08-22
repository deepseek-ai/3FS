#pragma once

#include "common/net/sync/ConnectionPool.h"
#include "common/serde/ClientContext.h"

namespace hf3fs::net::sync {

class Client {
 public:
  struct Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(default_timeout, kClientRequestDefaultTimeout);
    CONFIG_HOT_UPDATED_ITEM(default_send_retry_times, kDefaultMaxRetryTimes, ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(default_compression_level, 0u);
    CONFIG_HOT_UPDATED_ITEM(default_compression_threshold, 128_KB);
    CONFIG_OBJ(connection_pool, ConnectionPool::Config);
  };

  explicit Client(const Config &config)
      : config_(config),
        connectionPool_(config_.connection_pool()),
        clientConfigGuard_(config_.addCallbackGuard([&] { updateDefaultOptions(); })) {
    updateDefaultOptions();
  }

  auto serdeCtx(Address serverAddr) { return serde::ClientContext(connectionPool_, serverAddr, options_); }

 protected:
  void updateDefaultOptions() {
    auto options = std::make_shared<CoreRequestOptions>();
    options->timeout = config_.default_timeout();
    options->sendRetryTimes = config_.default_send_retry_times();
    options->compression = {config_.default_compression_level(), config_.default_compression_threshold()};
    options_ = std::move(options);
  }

 private:
  const Config &config_;
  ConnectionPool connectionPool_;
  std::unique_ptr<ConfigCallbackGuard> clientConfigGuard_;
  folly::atomic_shared_ptr<const CoreRequestOptions> options_{std::make_shared<CoreRequestOptions>()};
};

}  // namespace hf3fs::net::sync
