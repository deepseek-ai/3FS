#pragma once

#include <regex>

#include "common/monitor/Reporter.h"
#include "common/utils/ConfigBase.h"

namespace hf3fs::monitor {

class LogReporter : public Reporter {
 public:
  struct Config : ConfigBase<Config> {
    CONFIG_ITEM(ignore, std::string{});
  };

  LogReporter(const Config &config)
      : config_(config) {}

  Result<Void> init() final;

  Result<Void> commit(const std::vector<Sample> &samples) final;

 private:
  const Config &config_;
  std::optional<std::regex> ignore_;
};

}  // namespace hf3fs::monitor
