#pragma once

#include "common/utils/Coroutine.h"

namespace hf3fs::mgmtd {
struct MgmtdState;
class MgmtdTargetInfoPersister {
 public:
  explicit MgmtdTargetInfoPersister(MgmtdState &state);

  CoTask<void> run();

 private:
  MgmtdState &state_;
};
}  // namespace hf3fs::mgmtd
