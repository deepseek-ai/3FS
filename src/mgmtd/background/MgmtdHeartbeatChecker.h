#pragma once

#include "common/utils/Coroutine.h"

namespace hf3fs::mgmtd {
struct MgmtdState;
class MgmtdHeartbeatChecker {
 public:
  explicit MgmtdHeartbeatChecker(MgmtdState &state);

  CoTask<void> check();

 private:
  MgmtdState &state_;
};
}  // namespace hf3fs::mgmtd
