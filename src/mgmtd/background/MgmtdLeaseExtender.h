#pragma once

#include "common/utils/Coroutine.h"

namespace hf3fs::mgmtd {
struct MgmtdState;
class MgmtdLeaseExtender {
 public:
  explicit MgmtdLeaseExtender(MgmtdState &state);

  CoTask<void> extend();

 private:
  MgmtdState &state_;
};
}  // namespace hf3fs::mgmtd
