#pragma once

#include <functional>

#include "common/utils/String.h"

namespace hf3fs::client::cli {
class Printer {
 public:
  using OutputHandler = std::function<void(const String &)>;

  explicit Printer(OutputHandler handler)
      : stdout_(std::move(handler)),
        stderr_(stdout_) {}

  Printer(OutputHandler stdoutHandler, OutputHandler stderrHandler)
      : stdout_(std::move(stdoutHandler)),
        stderr_(stderrHandler) {}

  void print(const String &s) const { stdout_(s); }
  void printError(const String &s) const { stderr_(s); }
  void print(const std::vector<std::vector<String>> &table) const;

 private:
  OutputHandler stdout_;
  OutputHandler stderr_;
};
}  // namespace hf3fs::client::cli
