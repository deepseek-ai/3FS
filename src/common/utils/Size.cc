#include "common/utils/Size.h"

#include <scn/scan/scan.h>
#include <scn/tuple_return.h>

namespace hf3fs {
namespace {

const std::unordered_map<std::string_view, uint64_t> kUnitMap = {
    {Size::kLabelB, Size::kUnitB},
    {Size::kLabelKB, Size::kUnitKB},
    {Size::kLabelMB, Size::kUnitMB},
    {Size::kLabelGB, Size::kUnitGB},
    {Size::kLabelTB, Size::kUnitTB},

    {Size::kLabelK, Size::kUnitK},
    {Size::kLabelM, Size::kUnitM},
    {Size::kLabelG, Size::kUnitG},
    {Size::kLabelT, Size::kUnitT},
};
constexpr std::string_view kLabelInfinity = "infinity";

}  // namespace

Result<Size> Size::from(std::string_view input) {
  if (input == kLabelInfinity) {
    return infinity();
  }
  auto [r, i, s] = scn::scan_tuple<uint64_t, std::string>(input, "{}{}");
  if (!r) {
    r = scn::scan(input, "{}", i);
    s = kLabelB;
  }
  if (!r) {
    return makeError(StatusCode::kInvalidConfig, "wrong duration format");
  }

  auto it = kUnitMap.find(s);
  if (it == kUnitMap.end()) {
    return makeError(StatusCode::kInvalidConfig, "wrong duration unit");
  }
  return Size{i * it->second};
}

static std::string toStringBase1024(uint64_t s) {
  auto level = __builtin_ctzll(s) / 10;
  switch (level) {
    case 0:
      return fmt::format("{}B", s);
    case 1:
      return fmt::format("{}{}", s / Size::kUnitKB, Size::kLabelKB);
    case 2:
      return fmt::format("{}{}", s / Size::kUnitMB, Size::kLabelMB);
    case 3:
      return fmt::format("{}{}", s / Size::kUnitGB, Size::kLabelGB);
    default:
      return fmt::format("{}{}", s / Size::kUnitTB, Size::kLabelTB);
  }
}

static std::string toStringBase1000(uint64_t s) {
  if (s % Size::kUnitK == 0) {
    if (s % Size::kUnitT == 0) {
      return fmt::format("{}{}", s / Size::kUnitT, Size::kLabelT);
    } else if (s % Size::kUnitG == 0) {
      return fmt::format("{}{}", s / Size::kUnitG, Size::kLabelG);
    } else if (s % Size::kUnitM == 0) {
      return fmt::format("{}{}", s / Size::kUnitM, Size::kLabelM);
    } else {
      return fmt::format("{}{}", s / Size::kUnitK, Size::kLabelK);
    }
  }
  return fmt::format("{}", s);
}

std::string Size::toString(uint64_t s) {
  if (s == 0) {
    return "0";
  }
  if (s == infinity()) {
    return std::string{kLabelInfinity};
  }

  auto base1024 = toStringBase1024(s);
  auto base1000 = toStringBase1000(s);
  return base1000.length() < base1024.length() ? base1000 : base1024;
}

std::string Size::around(uint64_t s) {
  // use higher units if the size is greater than or equal to 512 current units.
  auto level = (std::numeric_limits<uint64_t>::digits - __builtin_clzll(s | 1)) / 10;
  switch (level) {
    case 0:
      return fmt::format("{}", s);

    case 1:
      return fmt::format("{:.2f}{}", double(s) / kUnitKB, kLabelKB);

    case 2:
      return fmt::format("{:.2f}{}", double(s) / kUnitMB, kLabelMB);

    case 3:
      return fmt::format("{:.2f}{}", double(s) / kUnitGB, kLabelGB);

    default:
      return fmt::format("{:.2f}{}", double(s) / kUnitTB, kLabelTB);
  }
}

}  // namespace hf3fs
