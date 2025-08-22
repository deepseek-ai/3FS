#include "monitor_collector/service/MonitorCollectorService.h"

#include <chrono>

#include "monitor_collector/service/MonitorCollectorOperator.h"

namespace hf3fs::monitor {

CoTryTask<MonitorCollectorRsp> MonitorCollectorService::write(serde::CallContext &ctx, std::vector<Sample> &samples) {
  co_await monitorCollectorOperator_.write(std::move(samples));
  co_return MonitorCollectorRsp{};
}

}  // namespace hf3fs::monitor
