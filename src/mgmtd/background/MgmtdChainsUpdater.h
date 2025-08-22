#pragma once

#include "common/utils/Coroutine.h"
#include "common/utils/UtcTime.h"

namespace hf3fs::mgmtd {
struct MgmtdState;
class MgmtdChainsUpdater {
 public:
  explicit MgmtdChainsUpdater(MgmtdState &state);

  CoTask<void> update(SteadyTime &lastUpdateTs);

 private:
  bool lastAdjustTargetOrderFlag = false;
  MgmtdState &state_;
};
}  // namespace hf3fs::mgmtd
