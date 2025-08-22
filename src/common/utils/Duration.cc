#include "common/utils/Duration.h"

#include <folly/String.h>
#include <scn/tuple_return.h>
#include <unordered_map>

namespace hf3fs {
namespace {

constexpr std::chrono::nanoseconds kUnitNanoSec = std::chrono::nanoseconds(1);
constexpr std::chrono::nanoseconds kUnitMicroSec = std::chrono::microseconds(1);
constexpr std::chrono::nanoseconds kUnitMilliSec = std::chrono::milliseconds(1);
constexpr std::chrono::nanoseconds kUnitSec = std::chrono::seconds(1);
constexpr std::chrono::nanoseconds kUnitMin = std::chrono::minutes(1);
constexpr std::chrono::nanoseconds kUnitHour = std::chrono::hours(1);
constexpr std::chrono::nanoseconds kUnitDay = std::chrono::days(1);

constexpr std::string_view kLabelNanoSec = "ns";
constexpr std::string_view kLabelMicroSec = "us";
constexpr std::string_view kLabelMilliSec = "ms";
constexpr std::string_view kLabelSec = "s";
constexpr std::string_view kLabelMin = "min";
constexpr std::string_view kLabelHour = "h";
constexpr std::string_view kLabelDay = "day";

const std::unordered_map<std::string_view, std::chrono::nanoseconds> kUnitMap = {
    {kLabelNanoSec, kUnitNanoSec},
    {kLabelMicroSec, kUnitMicroSec},
    {kLabelMilliSec, kUnitMilliSec},
    {kLabelSec, kUnitSec},
    {kLabelMin, kUnitMin},
    {kLabelHour, kUnitHour},
    {kLabelDay, kUnitDay},
};

}  // namespace

Result<Duration> Duration::from(std::string_view input) {
  std::chrono::nanoseconds ns(0);
  std::chrono::nanoseconds lastUnit = kUnitDay * 10;  // ensure initial lastUnit larger than any valid unit
  auto remaining = folly::trimWhitespace(input);
  if (remaining.empty()) {
    return makeError(StatusCode::kInvalidConfig, "empty duration string");
  }
  while (!remaining.empty()) {
    auto [r, i, s] = scn::scan_tuple<int64_t, std::string>(remaining, "{}{}");
    if (!r) {
      return makeError(StatusCode::kInvalidConfig, fmt::format("wrong duration format '{}'", input));
    }
    auto it = kUnitMap.find(s);
    if (it == kUnitMap.end()) {
      return makeError(StatusCode::kInvalidConfig, fmt::format("wrong duration format '{}'", input));
    }
    auto unit = it->second;
    if (unit >= lastUnit) {
      return makeError(StatusCode::kInvalidConfig, fmt::format("wrong duration format '{}'", input));
    }
    lastUnit = unit;
    ns += i * unit;
    assert(r.range().empty() || r.range().is_contiguous);
    if (r.range().empty())
      break;
    else
      remaining = folly::trimWhitespace(std::string_view(r.range().data(), r.range().size()));
  }
  return Duration{ns};
}

std::string Duration::toString() const {
  uint64_t v = count();
  if (v == 0) {
    return "0ns";
  }

  std::string result;
  while (v != 0) {
    std::string_view label;
    if (v >= kUnitSec.count()) {
      if (v >= kUnitDay.count()) {
        label = kLabelDay;
      } else if (v >= kUnitHour.count()) {
        label = kLabelHour;
      } else if (v >= kUnitMin.count()) {
        label = kLabelMin;
      } else {
        label = kLabelSec;
      }
    } else if (v >= kUnitMilliSec.count()) {
      label = kLabelMilliSec;
    } else if (v >= kUnitMicroSec.count()) {
      label = kLabelMicroSec;
    } else {
      label = kLabelNanoSec;
    }
    auto unit = kUnitMap.at(label);
    fmt::format_to(std::back_inserter(result), "{}{} ", v / unit.count(), label);
    v %= unit.count();
  }
  if (result.back() == ' ') {
    result.pop_back();
  }
  return result;
}

}  // namespace hf3fs
