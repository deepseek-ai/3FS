#pragma once

#include "common/net/Server.h"
#include "monitor_collector/service/MonitorCollectorService.h"

namespace hf3fs::monitor {

class MonitorCollectorServer : public net::Server {
 public:
  static constexpr auto kName = "MonitorCollector";

  struct Config : public ConfigBase<Config> {
    CONFIG_OBJ(base, net::Server::Config, [](net::Server::Config &c) {
      c.set_groups_length(1);
      c.groups(0).listener().set_listen_port(10000);
      c.groups(0).set_network_type(hf3fs::net::Address::TCP);
      c.groups(0).set_services({"MonitorCollector"});
    });
    CONFIG_OBJ(monitor_collector, monitor::MonitorCollectorService::Config);
  };

  MonitorCollectorServer(const Config &config);
  ~MonitorCollectorServer() override;

  // set up monitor collector server.
  Result<Void> beforeStart() final;

  // tear down monitor collector server.
  Result<Void> beforeStop() final;

 private:
  std::unique_ptr<MonitorCollectorOperator> monitorCollectorOperator_;
  const Config &config_;
};

}  // namespace hf3fs::monitor
