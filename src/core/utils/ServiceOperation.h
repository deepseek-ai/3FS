#pragma once

#include <fmt/format.h>
#include <folly/experimental/symbolizer/Symbolizer.h>
#include <folly/logging/xlog.h>

#include "common/monitor/Recorder.h"
#include "common/utils/NameWrapper.h"
#include "common/utils/String.h"

#define RETURN_AND_LOG_OP_ERR(...) RETURN_AND_LOG_OP_ERR_IMPL(return, __VA_ARGS__)
#define CO_RETURN_AND_LOG_OP_ERR(...) RETURN_AND_LOG_OP_ERR_IMPL(co_return, __VA_ARGS__)

#define RETURN_AND_LOG_OP_ERR_IMPL(RETURN, op, code, s, ...)            \
  do {                                                                  \
    auto _msg = fmt::format(s __VA_OPT__(, ) __VA_ARGS__);              \
    LOG_OP_ERR((op), "error: {} {}", StatusCode::toString(code), _msg); \
    RETURN makeError(code, _msg);                                       \
  } while (false)

#define LOG_OP_DBG(...) LOG_OP_IMPL(DBG, __VA_ARGS__)
#define LOG_OP_INFO(...) LOG_OP_IMPL(INFO, __VA_ARGS__)
#define LOG_OP_WARN(...) LOG_OP_IMPL(WARN, __VA_ARGS__)
#define LOG_OP_ERR(...) LOG_OP_IMPL(ERR, __VA_ARGS__)
#define LOG_OP_CRITICAL(...) LOG_OP_IMPL(CRITICAL, __VA_ARGS__)
#define LOG_OP_FATAL(op, fmt, ...)                  \
  XLOGF(FATAL,                                      \
        "{} " fmt ". stack trace:\n{}",             \
        (op).toString() __VA_OPT__(, ) __VA_ARGS__, \
        folly::symbolizer::getStackTraceStr())

#define LOG_OP_IMPL(LEVEL, op, fmt, ...) XLOGF(LEVEL, "{} " fmt, (op).toString() __VA_OPT__(, ) __VA_ARGS__)

namespace hf3fs::core {
struct ServiceOperation {
  virtual ~ServiceOperation() = default;

  String toString() const {
    if (!cache) {
      cache = fmt::format("[{}Op {} No.{}]", serviceNameImpl(), toStringImpl(), reqId);
    }
    return *cache;
  }

  template <typename T>
  requires(std::is_base_of_v<ServiceOperation, T>) T &as() {
    auto *p = dynamic_cast<T *>(this);
    XLOG_IF(FATAL, !p, "invalid dynamic cast from ServiceOperation to {}", typeid(T).name());
    return *p;
  }

  template <typename T>
  requires(std::is_base_of_v<ServiceOperation, T>) const T &as() const {
    auto *p = dynamic_cast<const T *>(this);
    XLOG_IF(FATAL, !p, "invalid dynamic cast from ServiceOperation to {}", typeid(T).name());
    return *p;
  }

 private:
  virtual String serviceNameImpl() const = 0;
  virtual String toStringImpl() const = 0;

  static int64_t nextReqId() {
    static std::atomic<int64_t> id{1};
    return id.fetch_add(1, std::memory_order_acq_rel);
  }

  const int64_t reqId = nextReqId();
  mutable std::optional<String> cache;
};

template <NameWrapper ServiceName, NameWrapper OpName, NameWrapper MetricNamePrefix>
struct ServiceOperationWithMetric : ServiceOperation {
  static constexpr std::string_view serviceName = ServiceName;
  static constexpr std::string_view opName = OpName;
  static constexpr std::string_view metricNamePrefix = MetricNamePrefix;

  static_assert(!serviceName.empty());
  static_assert(!opName.empty());
  static_assert(!metricNamePrefix.empty());

  static const monitor::TagSet &getTagSet() {
    static auto tagSet = [] {
      monitor::TagSet ts;
      ts.addTag("instance", String(opName));
      return ts;
    }();
    return tagSet;
  }

  static monitor::SimpleOperationRecorder::Guard startRecord() {
    static monitor::SimpleOperationRecorder recorder(fmt::format("{}.{}", serviceName, metricNamePrefix),
                                                     getTagSet(),
                                                     /*recordErrorCode=*/true);
    return recorder.record();
  }

  String serviceNameImpl() const final { return String(serviceName); }
};

}  // namespace hf3fs::core
