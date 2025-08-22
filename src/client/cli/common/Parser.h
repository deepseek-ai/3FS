#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/utils/Result.h"

namespace hf3fs::client::cli {
class ArgsParser {
 public:
  explicit ArgsParser(std::string_view s)
      : s_(s) {}

  Result<std::string> parseOne();
  Result<std::vector<std::string>> parseAll();

 private:
  std::string_view s_;
};
}  // namespace hf3fs::client::cli
