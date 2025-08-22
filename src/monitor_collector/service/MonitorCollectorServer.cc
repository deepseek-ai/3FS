#include "monitor_collector/service/MonitorCollectorServer.h"

#include "monitor_collector/service/MonitorCollectorOperator.h"

namespace hf3fs::monitor {

MonitorCollectorServer::MonitorCollectorServer(const MonitorCollectorServer::Config &config)
    : net::Server(config.base()),
      config_(config) {}

MonitorCollectorServer::~MonitorCollectorServer() { XLOGF(INFO, "Destructor MonitorCollectorServer"); }

Result<Void> MonitorCollectorServer::beforeStart() {
  monitorCollectorOperator_ = std::make_unique<MonitorCollectorOperator>(config_.monitor_collector());
  RETURN_ON_ERROR(addSerdeService(std::make_unique<MonitorCollectorService>(*monitorCollectorOperator_), true));
  return Void{};
}

Result<Void> MonitorCollectorServer::beforeStop() {
  monitorCollectorOperator_.reset();
  return Void{};
}

}  // namespace hf3fs::monitor
