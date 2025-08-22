#include "common/monitor/LogReporter.h"

#include <folly/logging/xlog.h>
#include <regex>

#include "common/utils/Size.h"

namespace hf3fs::monitor {

Result<Void> LogReporter::init() {
  auto ignore = config_.ignore();
  if (!ignore.empty()) {
    ignore_ = std::regex(ignore);
  }
  return Void{};
}

Result<Void> LogReporter::commit(const std::vector<Sample> &samples) {
  XLOGF(INFO, "");
  for (auto &sample : samples) {
    if (ignore_.has_value() && std::regex_search(sample.name, ignore_.value())) {
      continue;
    }

    if (sample.isNumber()) {
      if (sample.name.find("bytes") != std::string::npos) {
        XLOGF(INFO, "{}: {}", sample.name, Size::around(sample.number()));
      } else {
        XLOGF(INFO, "{}: {}", sample.name, sample.number());
      }
    } else {
      auto base = 1;
      auto unit = "";
      if (sample.name.find("latency") != std::string::npos) {
        base = 1000;
        unit = " us";
      } else if (sample.name.find("bytes") != std::string::npos) {
        base = 1 << 10;
        unit = " KB";
      }
      auto &dist = sample.dist();
      XLOGF(INFO,
            "{0}: count:{1} "
            "mean:{3:.1f}{2} "
            "min:{4:.1f}{2} "
            "max:{5:.1f}{2} "
            "p50:{6:.1f}{2} "
            "p90:{7:.1f}{2} "
            "p95:{8:.1f}{2} "
            "p99:{9:.1f}{2}",
            sample.name,
            dist.cnt,
            unit,
            dist.mean() / base,
            dist.min / base,
            dist.max / base,
            dist.p50 / base,
            dist.p90 / base,
            dist.p95 / base,
            dist.p99 / base);
    }
  }
  return Void{};
}

}  // namespace hf3fs::monitor
