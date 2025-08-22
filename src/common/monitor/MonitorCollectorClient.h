#pragma once

#include "common/monitor/LogReporter.h"
#include "common/monitor/Reporter.h"
#include "common/monitor/Sample.h"
#include "common/net/Client.h"
#include "common/utils/Address.h"
#include "common/utils/ConfigBase.h"

namespace hf3fs::monitor {

class MonitorCollectorClient : public Reporter {
 public:
  struct Config : ConfigBase<Config> {
    CONFIG_ITEM(remote_ip, "");  // format: 127.0.0.1:10000
    CONFIG_OBJ(client, net::Client::Config);
  };

  MonitorCollectorClient(const Config &config)
      : config_(config) {}
  ~MonitorCollectorClient() override { stop(); }

  Result<Void> init() final;
  void stop();

  Result<Void> commit(const std::vector<Sample> &samples) final;

 private:
  const Config &config_;
  std::unique_ptr<net::Client> client_;
  std::unique_ptr<serde::ClientContext> ctx_;
};

}  // namespace hf3fs::monitor
