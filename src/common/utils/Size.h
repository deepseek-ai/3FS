#pragma once

#include <fmt/core.h>
#include <limits>
#include <string>
#include <string_view>

#include "common/utils/Result.h"

namespace hf3fs {

class Size {
 public:
  static constexpr uint64_t kUnitB = 1ul << 0;
  static constexpr uint64_t kUnitKB = 1ul << 10;
  static constexpr uint64_t kUnitMB = 1ul << 20;
  static constexpr uint64_t kUnitGB = 1ul << 30;
  static constexpr uint64_t kUnitTB = 1ul << 40;

  static constexpr uint64_t kUnitK = 1000ull;
  static constexpr uint64_t kUnitM = 1000'000ull;
  static constexpr uint64_t kUnitG = 1000'000'000ull;
  static constexpr uint64_t kUnitT = 1000'000'000'000ull;

  static constexpr std::string_view kLabelB = "B";
  static constexpr std::string_view kLabelKB = "KB";
  static constexpr std::string_view kLabelMB = "MB";
  static constexpr std::string_view kLabelGB = "GB";
  static constexpr std::string_view kLabelTB = "TB";

  static constexpr std::string_view kLabelK = "K";
  static constexpr std::string_view kLabelM = "M";
  static constexpr std::string_view kLabelG = "G";
  static constexpr std::string_view kLabelT = "T";

  using is_serde_copyable = void;

  // allow implicit conversion
  constexpr Size(uint64_t value = 0) noexcept
      : value_(value) {}
  constexpr operator uint64_t() const { return value_; }
  constexpr uint64_t toInt() const { return value_; }
  std::string toString() const { return toString(value_); }
  std::string serdeToReadable() const { return toString(); }
  static Result<Size> serdeFromReadable(std::string_view input) { return from(input); }
  std::string around() const { return around(value_); }

  static Result<Size> from(std::string_view input);
  static std::string toString(uint64_t s);
  static std::string around(uint64_t s);

  static constexpr Size infinity() { return std::numeric_limits<uint64_t>::max(); }

 private:
  uint64_t value_ = 0;
};

inline constexpr Size operator""_B(unsigned long long value) { return Size(value * Size::kUnitB); }
inline constexpr Size operator""_KB(unsigned long long value) { return Size(value * Size::kUnitKB); }
inline constexpr Size operator""_MB(unsigned long long value) { return Size(value * Size::kUnitMB); }
inline constexpr Size operator""_GB(unsigned long long value) { return Size(value * Size::kUnitGB); }
inline constexpr Size operator""_TB(unsigned long long value) { return Size(value * Size::kUnitTB); }

inline constexpr Size operator""_K(unsigned long long value) { return Size(value * Size::kUnitK); }
inline constexpr Size operator""_M(unsigned long long value) { return Size(value * Size::kUnitM); }
inline constexpr Size operator""_G(unsigned long long value) { return Size(value * Size::kUnitG); }
inline constexpr Size operator""_T(unsigned long long value) { return Size(value * Size::kUnitT); }

}  // namespace hf3fs

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::Size> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::Size &size, FormatContext &ctx) const {
    return formatter<std::string_view>::format(size.toString(), ctx);
  }
};

FMT_END_NAMESPACE
