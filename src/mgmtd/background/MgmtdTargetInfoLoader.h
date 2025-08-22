#pragma once

#include "common/utils/Coroutine.h"
#include "common/utils/UtcTime.h"

namespace hf3fs::mgmtd {
struct MgmtdState;
class MgmtdTargetInfoLoader {
 public:
  explicit MgmtdTargetInfoLoader(MgmtdState &state);

  CoTask<void> run();

 private:
  SteadyTime loadedLeaseStart;
  MgmtdState &state_;
};
}  // namespace hf3fs::mgmtd
