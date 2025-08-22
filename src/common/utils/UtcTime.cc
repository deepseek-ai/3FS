#include <common/utils/UtcTime.h>
#include <ctime>

namespace hf3fs {
UtcTime UtcClock::now() noexcept {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  int64_t val = static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
  return UtcTime::fromMicroseconds(val);
}

std::time_t UtcClock::to_time_t(const UtcTime &t) noexcept {
  return std::time_t(std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
}

std::time_t UtcClock::secondsSinceEpoch() noexcept { return to_time_t(now()); }

UtcTime UtcTime::now() noexcept { return UtcClock::now(); }

std::string UtcTime::serdeToReadable() const { return fmt::to_string(*this); }

std::string UtcTime::YmdHMS() const {
  if (toMicroseconds() == 0) return "N/A";
  return fmt::format("{:%Y-%m-%d %H:%M:%S}", *this);
}

bool UtcTime::isZero() const { return *this == UtcTime{}; }

}  // namespace hf3fs
