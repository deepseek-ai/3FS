#include "Printer.h"

namespace hf3fs::client::cli {
void Printer::print(const std::vector<std::vector<String>> &table) const {
  static constexpr std::string_view separator = "  ";
  std::vector<size_t> widths;
  for (const auto &r : table) {
    if (widths.size() < r.size()) {
      widths.resize(r.size(), 0);
    }
    for (size_t col = 0; col < r.size(); ++col) {
      widths[col] = std::max(widths[col], r[col].size() + separator.size());
    }
  }
  for (const auto &r : table) {
    String s;
    for (size_t col = 0; col < r.size(); ++col) {
      s.append(r[col]);
      if (col + 1 < r.size()) {
        s.append(widths[col] - r[col].size(), ' ');
      }
    }
    stdout_(s);
  }
}
}  // namespace hf3fs::client::cli
