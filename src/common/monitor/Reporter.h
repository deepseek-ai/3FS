#pragma once

#include <vector>

#include "common/monitor/Sample.h"
#include "common/utils/Result.h"

namespace hf3fs::monitor {

class Reporter {
 public:
  virtual ~Reporter() = default;
  virtual Result<Void> init() = 0;
  virtual Result<Void> commit(const std::vector<Sample> &samples) = 0;
};

}  // namespace hf3fs::monitor
