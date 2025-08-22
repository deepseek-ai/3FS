#pragma once

#include "common/monitor/Sample.h"
#include "common/serde/Serde.h"
#include "common/serde/Service.h"

namespace hf3fs::monitor {

struct MonitorCollectorRsp {
  SERDE_STRUCT_FIELD(dummy, Void{});
};

SERDE_SERVICE(MonitorCollector, 194) { SERDE_SERVICE_METHOD(write, 1, std::vector<Sample>, MonitorCollectorRsp); };

}  // namespace hf3fs::monitor
