#include "updateChain.h"

#include "common/utils/OptionalUtils.h"

namespace hf3fs::mgmtd {
namespace {
using PS = enum flat::PublicTargetState;
using LS = enum flat::LocalTargetState;
using TargetsByPs = robin_hood::unordered_map<PS, std::vector<ChainTargetInfoEx>>;

void dispatch(TargetsByPs &targetsByPs, ChainTargetInfoEx ti, PS ps, std::string_view reason) {
  XLOGF_IF(DBG,
           ti.publicState != ps,
           "Dispatch {}({}-{}) to {}: {}",
           ti.targetId,
           magic_enum::enum_name(ti.publicState),
           magic_enum::enum_name(ti.localState),
           magic_enum::enum_name(ps),
           reason);
  ti.publicState = ps;
  targetsByPs[ps].push_back(ti);
}
}  // namespace

std::vector<ChainTargetInfoEx> generateNewChain(const std::vector<ChainTargetInfoEx> &oldTargets) {
  robin_hood::unordered_map<PS, std::vector<ChainTargetInfoEx>> oldTargetsByPs, newTargetsByPs;
  for (const auto &ti : oldTargets) {
    oldTargetsByPs[ti.publicState].push_back(ti);
  }

  for (const auto &ti : oldTargetsByPs[PS::SERVING]) {
    if (ti.localState == LS::ONLINE || ti.localState == LS::UPTODATE) {
      dispatch(newTargetsByPs, ti, PS::SERVING, "");
    } else if (newTargetsByPs[PS::LASTSRV].empty()) {
      // If all SERVING offlined, only the first becomes LASTSRV.
      // NOTE: in such cases the whole chain has to wait the HEAD for recovering even
      //       when other replicas are complete.
      dispatch(newTargetsByPs, ti, PS::LASTSRV, "first SERVING");
    } else {
      dispatch(newTargetsByPs, ti, PS::OFFLINE, "following SERVINGs");
    }
  }

  for (const auto &ti : oldTargetsByPs[PS::LASTSRV]) {
    if (newTargetsByPs[PS::SERVING].empty()) {
      if (ti.localState == LS::ONLINE || ti.localState == LS::UPTODATE) {
        dispatch(newTargetsByPs, ti, PS::SERVING, "first LASTSRV");
      } else {
        dispatch(newTargetsByPs, ti, PS::LASTSRV, "following LASTSRVs");
      }
    } else {
      dispatch(newTargetsByPs, ti, PS::OFFLINE, "Has SERVING");
    }
  }

  for (const auto &ti : oldTargetsByPs[PS::SYNCING]) {
    if (ti.localState == LS::UPTODATE) {
      dispatch(newTargetsByPs, ti, PS::SERVING, "");
    } else if (ti.localState == LS::ONLINE) {
      if (!newTargetsByPs[PS::SERVING].empty()) {
        dispatch(newTargetsByPs, ti, PS::SYNCING, "Has SERVING");
      } else {
        dispatch(newTargetsByPs, ti, PS::WAITING, "No SERVING");
      }
    } else {
      dispatch(newTargetsByPs, ti, PS::OFFLINE, "");
    }
  }

  for (const auto &ti : oldTargetsByPs[PS::WAITING]) {
    if (!newTargetsByPs[PS::SERVING].empty() && newTargetsByPs[PS::SYNCING].empty() && ti.localState == LS::ONLINE) {
      dispatch(newTargetsByPs, ti, PS::SYNCING, "Has SERVING && No SYNCING");
    } else if (ti.localState == LS::ONLINE || ti.localState == LS::UPTODATE) {
      dispatch(newTargetsByPs, ti, PS::WAITING, "");
    } else {
      dispatch(newTargetsByPs, ti, PS::OFFLINE, "");
    }
  }

  for (const auto &ti : oldTargetsByPs[PS::OFFLINE]) {
    if (!newTargetsByPs[PS::SERVING].empty() && newTargetsByPs[PS::SYNCING].empty() && ti.localState == LS::ONLINE) {
      dispatch(newTargetsByPs, ti, PS::SYNCING, "Has SERVING && No SYNCING");
    } else if (ti.localState == LS::ONLINE || ti.localState == LS::UPTODATE) {
      dispatch(newTargetsByPs, ti, PS::WAITING, "");
    } else {
      dispatch(newTargetsByPs, ti, PS::OFFLINE, "");
    }
  }

  if (!newTargetsByPs[PS::SERVING].empty()) {
    for (auto &ti : newTargetsByPs[PS::LASTSRV]) {
      dispatch(newTargetsByPs, std::move(ti), PS::OFFLINE, "Has SERVING");
    }
    newTargetsByPs[PS::LASTSRV].clear();
  }

  std::vector<ChainTargetInfoEx> newTargets;
  for (auto s : {PS::SERVING, PS::LASTSRV, PS::SYNCING, PS::WAITING, PS::OFFLINE}) {
    const auto &v = newTargetsByPs[s];
    newTargets.insert(newTargets.end(), v.begin(), v.end());
  }
  assert(oldTargets.size() == newTargets.size());
  return newTargets;
}

std::vector<ChainTargetInfoEx> rotateAsPreferredOrder(const std::vector<ChainTargetInfoEx> &oldTargets,
                                                      const std::vector<flat::TargetId> &preferredOrder) {
  XLOGF_IF(DFATAL,
           oldTargets.size() < preferredOrder.size(),
           "oldTargets.size = {}, preferredTargets.size = {}",
           oldTargets.size(),
           preferredOrder.size());
  std::map<flat::TargetId, std::pair<size_t, ChainTargetInfoEx>> oldMapping;
  for (size_t i = 0; i < oldTargets.size(); ++i) {
    oldMapping[oldTargets[i].targetId] = std::make_pair(i, oldTargets[i]);
  }
  for (size_t i = 0; i < preferredOrder.size(); ++i) {
    auto tid = preferredOrder[i];
    XLOGF_IF(DFATAL, !oldMapping.contains(tid), "{} not in oldTargets", tid);
    const auto &[oldPos, oldInfo] = oldMapping[tid];
    if (oldPos == i) {
      continue;
    }
    if (oldInfo.publicState != PS::SERVING) {
      break;
    }
    std::vector<ChainTargetInfoEx> newTargets;
    for (size_t j = 0; j < i; ++j) {
      newTargets.push_back(oldTargets[j]);
    }
    for (size_t j = i + 1; j < oldTargets.size(); ++j) {
      newTargets.push_back(oldTargets[j]);
    }
    auto info = oldTargets[i];
    info.publicState = PS::OFFLINE;
    info.localState = LS::OFFLINE;
    newTargets.push_back(info);
    return newTargets;
  }
  return oldTargets;
}

std::vector<ChainTargetInfoEx> rotateLastSrv(const std::vector<ChainTargetInfoEx> &oldTargets) {
  if (oldTargets.size() < 2 || oldTargets[0].publicState != PS::LASTSRV) {
    return oldTargets;
  }

  std::vector<ChainTargetInfoEx> newTargets;
  for (size_t i = 1; i < oldTargets.size(); ++i) newTargets.push_back(oldTargets[i]);
  newTargets.push_back(oldTargets[0]);

  // possible conversions:
  // * WAITING -> LASTSRV: it's safer to convert a WAITING to LASTSRV than to SERVING
  // * OFFLINE -> LASTSRV
  newTargets[0].publicState = PS::LASTSRV;

  // possible conversions:
  // * WAITING -> OFFLINE
  // * OFFLINE -> OFFLINE
  for (size_t i = 1; i < newTargets.size(); ++i) newTargets[i].publicState = PS::OFFLINE;

  return newTargets;
}

std::vector<flat::ChainTargetInfo> shutdownChain(const std::vector<flat::ChainTargetInfo> &oldTargets) {
  std::vector<flat::ChainTargetInfo> newTargets;
  for (auto ti : oldTargets) {
    switch (ti.publicState) {
      case PS::SERVING:
        ti.publicState = PS::LASTSRV;
        break;
      case PS::WAITING:
      case PS::SYNCING:
        ti.publicState = PS::OFFLINE;
        break;
      case PS::LASTSRV:
      case PS::OFFLINE:
        // keep it as is
        break;
      case PS::INVALID:
        XLOGF(FATAL, "Invalid PublicTargetState: {}", ti.targetId);
    }
    newTargets.push_back(ti);
  }
  return newTargets;
}

}  // namespace hf3fs::mgmtd
