#pragma once

#include "common/utils/Coroutine.h"

namespace hf3fs::mgmtd {
struct MgmtdState;
class MgmtdClientSessionsChecker {
 public:
  explicit MgmtdClientSessionsChecker(MgmtdState &state);

  CoTask<void> check();

 private:
  MgmtdState &state_;
};
}  // namespace hf3fs::mgmtd
