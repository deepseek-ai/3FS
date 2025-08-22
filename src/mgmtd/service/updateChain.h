#pragma once

#include "LocalTargetInfoWithNodeId.h"
#include "fbs/mgmtd/ChainTargetInfo.h"
#include "fbs/mgmtd/TargetInfo.h"

namespace hf3fs::mgmtd {
struct ChainTargetInfoEx : public flat::ChainTargetInfo {
  ChainTargetInfoEx() = default;
  ChainTargetInfoEx(const flat::ChainTargetInfo &cti, flat::LocalTargetState ls)
      : flat::ChainTargetInfo(cti),
        localState(ls) {}
  explicit ChainTargetInfoEx(const flat::TargetInfo &ti) {
    targetId = ti.targetId;
    publicState = ti.publicState;
    localState = ti.localState;
  }

  bool operator==(const ChainTargetInfo &other) const { return static_cast<const ChainTargetInfo &>(*this) == other; }
  bool operator==(const ChainTargetInfoEx &other) const {
    return *this == static_cast<const ChainTargetInfo &>(other) && localState == other.localState;
  }

  static ChainTargetInfoEx fromTargetInfo(const flat::TargetInfo &ti) { return ChainTargetInfoEx(ti); }

  static flat::TargetInfo toTargetInfo(const ChainTargetInfoEx &cti) {
    flat::TargetInfo ti;
    ti.targetId = cti.targetId;
    ti.publicState = cti.publicState;
    ti.localState = cti.localState;
    return ti;
  }

  flat::LocalTargetState localState{flat::LocalTargetState::INVALID};
};

// separate this function just for testing friendly
std::vector<ChainTargetInfoEx> generateNewChain(const std::vector<ChainTargetInfoEx> &oldTargets);

std::vector<ChainTargetInfoEx> rotateAsPreferredOrder(const std::vector<ChainTargetInfoEx> &oldTargets,
                                                      const std::vector<flat::TargetId> &preferredOrder);

// If the head of oldTargets is LASTSRV, move it to the tail and let the next target be the new LASTSRV.
// It's used when a LASTSRV target could not recover in a short time. Admin could let the next target be the new LASTSRV
// to resume the service. NOTE: it's on risk of losing some data forever.
std::vector<ChainTargetInfoEx> rotateLastSrv(const std::vector<ChainTargetInfoEx> &oldTargets);

std::vector<flat::ChainTargetInfo> shutdownChain(const std::vector<flat::ChainTargetInfo> &oldTargets);
}  // namespace hf3fs::mgmtd
