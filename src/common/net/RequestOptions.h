#pragma once

#include <utility>

#include "common/serde/Serde.h"
#include "common/utils/Duration.h"
#include "common/utils/Size.h"

namespace hf3fs::net {

constexpr Duration kClientRequestDefaultTimeout = 1000_ms;
constexpr Duration kClientRequestLogLongRunningThreshold = 0_ms;
constexpr uint32_t kDefaultMaxRetryTimes = 1;

struct CompressionOptions {
  operator bool() const noexcept { return level; }
  bool enable(size_t size) const noexcept { return level && size >= threshold; }
  SERDE_STRUCT_FIELD(level, 0u);
  SERDE_STRUCT_FIELD(threshold, 0_B);
};

struct UserRequestOptions {
  SERDE_STRUCT_FIELD(timeout, std::optional<Duration>{});
  SERDE_STRUCT_FIELD(sendRetryTimes, std::optional<uint32_t>{});
  SERDE_STRUCT_FIELD(compression, std::optional<CompressionOptions>{});
  SERDE_STRUCT_FIELD(logLongRunningThreshold, std::optional<Duration>{});
  SERDE_STRUCT_FIELD(reportMetrics, std::optional<bool>{});
};

struct CoreRequestOptions {
  void merge(const UserRequestOptions &o) {
    if (o.timeout) {
      timeout = o.timeout.value();
    }
    if (o.sendRetryTimes) {
      sendRetryTimes = o.sendRetryTimes.value();
    }
    if (o.compression) {
      compression = o.compression.value();
    }
    if (o.logLongRunningThreshold) {
      logLongRunningThreshold = o.logLongRunningThreshold.value();
    }
    if (o.reportMetrics) {
      reportMetrics = o.reportMetrics.value();
    }
  }

  SERDE_STRUCT_FIELD(timeout, kClientRequestDefaultTimeout);
  SERDE_STRUCT_FIELD(sendRetryTimes, kDefaultMaxRetryTimes);
  SERDE_STRUCT_FIELD(compression, CompressionOptions{});
  SERDE_STRUCT_FIELD(enableRDMAControl, false);
  SERDE_STRUCT_FIELD(logLongRunningThreshold, kClientRequestLogLongRunningThreshold);
  SERDE_STRUCT_FIELD(reportMetrics, false);
};

}  // namespace hf3fs::net
