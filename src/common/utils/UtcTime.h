#pragma once

#include <chrono>
#include <cstdint>
#include <fmt/chrono.h>
#include <folly/Math.h>
#include <folly/io/async/HHWheelTimer.h>

#include "common/utils/Duration.h"

namespace hf3fs {
class UtcTime;
// UtcClock is similar to std::chrono::system_clock except that it's based on UTC.
// UtcClock only provides microsecond-level timestamps.
class UtcClock {
 public:
  using rep = int64_t;
  using period = std::ratio<1, 1000000>;
  using duration = std::chrono::duration<rep, period>;
  using time_point = UtcTime;
  static constexpr bool is_steady = false;

  static time_point now() noexcept;

  static std::time_t secondsSinceEpoch() noexcept;

  static std::time_t to_time_t(const time_point &t) noexcept;
};

class UtcTime : public std::chrono::time_point<UtcClock> {
  using Base = std::chrono::time_point<UtcClock>;

 public:
  using Base::Base;
  using is_serde_copyable = void;
  constexpr UtcTime()
      : Base(std::chrono::microseconds(0)) {}
  constexpr UtcTime(const Base &base)
      : Base(base) {}

  static constexpr UtcTime from(std::chrono::microseconds us) { return UtcTime(us); }
  static constexpr UtcTime fromMicroseconds(int64_t us) { return UtcTime(std::chrono::microseconds(us)); }
  constexpr int64_t toMicroseconds() const {
    return std::chrono::duration_cast<std::chrono::microseconds>(time_since_epoch()).count();
  }

  UtcTime castGranularity(Duration gran) const {
    auto us = toMicroseconds();
    auto res = std::max((int64_t)1, gran.asUs().count());
    return UtcTime::fromMicroseconds(us / res * res);
  }

  static UtcTime now() noexcept;

  std::string serdeToReadable() const;

  std::string YmdHMS() const;

  bool isZero() const;
};

using SteadyClock = std::chrono::steady_clock;
using SteadyTime = SteadyClock::time_point;
}  // namespace hf3fs

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::UtcTime> : formatter<std::tm> {
  FMT_CONSTEXPR formatter() { this->format_str = "%F %T"; }

  template <typename FormatContext>
  auto format(hf3fs::UtcTime val, FormatContext &ctx) const -> decltype(ctx.out()) {
    return formatter<std::tm>::format(localtime(hf3fs::UtcClock::to_time_t(val)), ctx);
  }
};

FMT_END_NAMESPACE
