#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/utils/Result.h"

namespace hf3fs {

class Duration : public std::chrono::nanoseconds {
 public:
  using is_serde_copyable = void;

  explicit constexpr Duration(std::chrono::nanoseconds duration = {})
      : std::chrono::nanoseconds(duration) {}

  auto asSec() const { return std::chrono::duration_cast<std::chrono::seconds>(*this); }
  auto asUs() const { return std::chrono::duration_cast<std::chrono::microseconds>(*this); }
  auto asMs() const { return std::chrono::duration_cast<std::chrono::milliseconds>(*this); }

  std::string toString() const;
  std::string serdeToReadable() const { return toString(); }

  auto operator+(Duration o) const { return Duration{std::chrono::nanoseconds(*this) + std::chrono::nanoseconds(o)}; }
  auto operator-(Duration o) const { return Duration{std::chrono::nanoseconds(*this) - std::chrono::nanoseconds(o)}; }
  auto operator*(auto times) const {
    return Duration{std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::nanoseconds(*this) * times)};
  }

  operator std::chrono::microseconds() const { return asUs(); }
  operator std::chrono::milliseconds() const { return asMs(); }

  constexpr auto operator<=>(const Duration &) const = default;

  static Result<Duration> from(std::string_view input);

  static Duration zero() { return Duration(std::chrono::nanoseconds::zero()); }
};

inline constexpr Duration operator""_ns(unsigned long long v) { return Duration(std::chrono::nanoseconds(v)); }
inline constexpr Duration operator""_us(unsigned long long v) { return Duration(std::chrono::microseconds(v)); }
inline constexpr Duration operator""_ms(unsigned long long v) { return Duration(std::chrono::milliseconds(v)); }
inline constexpr Duration operator""_s(unsigned long long v) { return Duration(std::chrono::seconds(v)); }
inline constexpr Duration operator""_min(unsigned long long v) { return Duration(std::chrono::minutes(v)); }
inline constexpr Duration operator""_h(unsigned long long v) { return Duration(std::chrono::hours(v)); }
inline constexpr Duration operator""_d(unsigned long long v) { return Duration(std::chrono::days(v)); }

class RelativeTime : public std::chrono::nanoseconds {
 public:
  static auto now() { return RelativeTime{std::chrono::steady_clock::now().time_since_epoch()}; }
  static auto zero() { return RelativeTime{std::chrono::nanoseconds::zero()}; }

  auto operator-(RelativeTime time) const {
    return Duration{std::chrono::nanoseconds(*this) - std::chrono::nanoseconds(time)};
  }

  auto operator+(Duration time) const {
    return RelativeTime{std::chrono::nanoseconds(*this) + std::chrono::nanoseconds(time)};
  }
  auto operator-(Duration time) const {
    return RelativeTime{std::chrono::nanoseconds(*this) - std::chrono::nanoseconds(time)};
  }
};

}  // namespace hf3fs

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::Duration> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::Duration &duration, FormatContext &ctx) const {
    return formatter<std::string_view>::format(duration.toString(), ctx);
  }
};

FMT_END_NAMESPACE
