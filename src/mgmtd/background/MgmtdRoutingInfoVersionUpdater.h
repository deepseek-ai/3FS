#pragma once

#include "common/utils/Coroutine.h"

namespace hf3fs::mgmtd {
struct MgmtdState;
class MgmtdRoutingInfoVersionUpdater {
 public:
  explicit MgmtdRoutingInfoVersionUpdater(MgmtdState &state);

  CoTask<void> update();

 private:
  MgmtdState &state_;
};
}  // namespace hf3fs::mgmtd
