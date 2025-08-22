#include "helpers.h"

namespace hf3fs::mgmtd {
CoTask<void> updateMemoryRoutingInfo(MgmtdState &state, core::ServiceOperation &ctx) {
  static const auto handler = [](RoutingInfo &) {};
  co_await updateMemoryRoutingInfo(state, ctx, handler);
}

CoTryTask<void> updateStoredRoutingInfo(MgmtdState &state, core::ServiceOperation &ctx) {
  static const auto handler = [](kv::IReadWriteTransaction &) -> CoTryTask<void> { co_return Void{}; };
  co_return co_await updateStoredRoutingInfo(state, ctx, handler);
}

flat::TargetInfo makeTargetInfo(flat::ChainId chainId, const flat::ChainTargetInfo &info) {
  flat::TargetInfo ti;
  ti.targetId = info.targetId;
  ti.publicState = info.publicState;
  ti.localState = flat::LocalTargetState::OFFLINE;
  ti.chainId = chainId;
  return ti;
}

Result<Void> updateSelfConfig(MgmtdState &state, const flat::ConfigInfo &cfg) {
  XLOGF_IF(FATAL,
           state.selfNodeInfo_.configVersion > cfg.configVersion,
           "ConfigVersion in memory ({}) is larger than given ({})",
           state.selfNodeInfo_.configVersion.toUnderType(),
           cfg.configVersion.toUnderType());
  if (state.selfNodeInfo_.configVersion >= cfg.configVersion) return Void{};

  const auto &updater = state.env_->configUpdater();
  if (!updater) {
    XLOGF(WARN, "Mgmtd: blindly promote to {} since no listener found", cfg.configVersion);
    return Void{};
  }
  return updater(cfg.content, cfg.genUpdateDesc());
}

Result<std::vector<flat::TagPair>> updateTags(core::ServiceOperation &op,
                                              flat::SetTagMode mode,
                                              const std::vector<flat::TagPair> &oldTags,
                                              const std::vector<flat::TagPair> &updates) {
  std::map<std::string_view, std::string_view> tagMap;
  switch (mode) {
    case flat::SetTagMode::REPLACE: {
      for (const auto &tp : updates) tagMap[tp.key] = tp.value;
      break;
    }
    case flat::SetTagMode::UPSERT: {
      for (const auto &tp : updates) tagMap[tp.key] = tp.value;
      for (const auto &tp : oldTags) tagMap.try_emplace(tp.key, tp.value);
      break;
    }
    case flat::SetTagMode::REMOVE: {
      for (const auto &tp : oldTags) tagMap[tp.key] = tp.value;
      for (const auto &tp : updates) {
        if (!tp.value.empty()) {
          RETURN_AND_LOG_OP_ERR(op,
                                MgmtdCode::kInvalidTag,
                                "could only pass empty value for REMOVE. key {} value {}",
                                tp.key,
                                tp.value);
        }
        tagMap.erase(tp.key);
      }
      break;
    }
  }

  std::vector<flat::TagPair> newTags;
  for (const auto &[k, v] : tagMap) {
    newTags.emplace_back(String(k), String(v));
  }
  return newTags;
}

void updateMemoryRoutingInfo(RoutingInfo &alreadyLockedRoutingInfo, core::ServiceOperation &ctx) {
  auto &ri = alreadyLockedRoutingInfo;
  ++ri.routingInfoVersion.toUnderType();
  ri.routingInfoChanged = false;
  LOG_OP_INFO(ctx, "RoutingInfo: bump memory version to {}", ri.routingInfoVersion);
}

CoTryTask<Void> ensureSelfIsPrimary(MgmtdState &state) {
  auto lease = co_await state.currentLease(state.utcNow());
  if (lease.has_value()) {
    if (lease->primary.nodeId == state.selfId()) {
      co_return Void{};
    } else {
      co_return makeError(MgmtdCode::kNotPrimary, fmt::format("{}", lease->primary.nodeId.toUnderType()));
    }
  }
  co_return makeError(MgmtdCode::kNotPrimary);
}
}  // namespace hf3fs::mgmtd
