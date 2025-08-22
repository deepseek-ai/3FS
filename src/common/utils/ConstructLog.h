#pragma once

#include <folly/logging/xlog.h>

#include "common/utils/NameWrapper.h"

namespace hf3fs {

template <NameWrapper Name>
struct ConstructLog {
  ConstructLog() { XLOGF(INFO, "Construct {}", std::string_view{Name}); }
  ~ConstructLog() { XLOGF(INFO, "Destruct {}", std::string_view{Name}); }
};

}  // namespace hf3fs
