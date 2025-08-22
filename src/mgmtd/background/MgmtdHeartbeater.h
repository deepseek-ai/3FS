#pragma once

#include "common/utils/Coroutine.h"

namespace hf3fs::mgmtd {
struct MgmtdState;
class MgmtdHeartbeater {
 public:
  explicit MgmtdHeartbeater(MgmtdState &state);
  ~MgmtdHeartbeater();

  CoTask<void> send();

  struct SendHeartbeatContext;

 private:
  MgmtdState &state_;

  std::unique_ptr<SendHeartbeatContext> sendHeartbeatCtx_;
};
}  // namespace hf3fs::mgmtd
