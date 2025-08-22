#pragma once

#include <cstring>
#include <folly/io/async/Request.h>
#include <memory>
#include <mutex>

#include "String.h"
#include "TracingEvent.h"
#include "UtcTime.h"

#define SCOPE_SET_TRACING_POINTS(points) hf3fs::tracing::ScopeGuard FB_ANONYMOUS_VARIABLE(guard)(points)

#define TRACING_ADD_SCOPE_EVENT(...) hf3fs::tracing::ScopeTracer FB_ANONYMOUS_VARIABLE(guard)(__VA_ARGS__)

#define TRACING_ADD_EVENT(...)                                       \
  do {                                                               \
    auto *proxy = hf3fs::tracing::PointsProxy::get();                \
    if (proxy) {                                                     \
      proxy->points().addPoint(hf3fs::UtcClock::now(), __VA_ARGS__); \
    }                                                                \
  } while (false)

namespace hf3fs::tracing {
struct Point {
  Point(UtcTime ts, uint64_t e, std::string_view msg = {})
      : timestamp_(ts),
        event_(e),
        message_(msg) {}

  ~Point() = default;

  Point(const Point &) = delete;
  Point(Point &&other) = default;

  Point &operator=(const Point &) = delete;
  Point &operator=(Point &&other) = default;

  void setMessage(std::string_view msg) { message_ = msg; }

  void clearMessage() { message_.clear(); }

  std::string_view message() const { return message_; }

  uint64_t event() const { return event_; }

  UtcTime timestamp() const { return timestamp_; }

 private:
  UtcTime timestamp_;
  uint64_t event_;
  String message_;
};

class Points {
 public:
  void addPoint(UtcTime ts, uint64_t event, std::string_view msg = {}) {
    std::unique_lock lock(mu_);
    points_.emplace_back(ts, event, msg);
  }

  std::vector<Point> extractAll() {
    std::unique_lock lock(mu_);
    return std::move(points_);
  }

 private:
  std::mutex mu_;
  std::vector<Point> points_;
};

class PointsProxy : public folly::RequestData {
 public:
  static PointsProxy *get() {
    auto requestContext = folly::RequestContext::try_get();
    if (LIKELY(requestContext == nullptr)) {
      return nullptr;
    }
    return dynamic_cast<PointsProxy *>(requestContext->getContextData(token()));
  }

  static const folly::RequestToken &token() {
    static const folly::RequestToken t(kTokenName);
    return t;
  }

  explicit PointsProxy(Points &points)
      : points_(points) {}

  Points &points() { return points_; }

  bool hasCallback() override { return false; }

 private:
  static constexpr auto kTokenName = "hf3fs::tracing::Points";

  Points &points_;
};

class ScopeGuard {
 public:
  explicit ScopeGuard(Points &points) { guard_.emplace(PointsProxy::token(), std::make_unique<PointsProxy>(points)); }

 private:
  std::optional<folly::ShallowCopyRequestContextScopeGuard> guard_;
};

class ScopeTracer {
 public:
  ScopeTracer(uint64_t event, std::string_view msg = {})
      : event_(event) {
    pointsProxy_ = PointsProxy::get();
    if (pointsProxy_) {
      pointsProxy_->points().addPoint(UtcClock::now(), getBeginEvent(event_), msg);
    }
  }

  ~ScopeTracer() {
    if (pointsProxy_) {
      pointsProxy_->points().addPoint(UtcClock::now(), getEndEvent(event_));
    }
  }

 private:
  PointsProxy *pointsProxy_;
  uint64_t event_;
};

}  // namespace hf3fs::tracing
