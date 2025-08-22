#include "RotateAsPreferredOrderOperation.h"

#include "common/utils/StringUtils.h"
#include "mgmtd/service/helpers.h"
#include "mgmtd/service/updateChain.h"

namespace hf3fs::mgmtd {
CoTryTask<RotateAsPreferredOrderRsp> RotateAsPreferredOrderOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  CO_RETURN_ON_ERROR(co_await state.validateAdmin(*this, req.user));

  if (req.chainId == 0) {
    CO_RETURN_AND_LOG_OP_ERR(*this, StatusCode::kInvalidArg, "Empty chain id");
  }

  auto handler = [&]() -> CoTryTask<RotateAsPreferredOrderRsp> {
    auto writerLock = co_await state.coScopedLock<"RotateAsPreferredOrder">();

    flat::ChainInfo chainInfo;
    std::vector<ChainTargetInfoEx> oldTargets;
    {
      auto dataPtr = co_await state.data_.coLock();
      auto &ri = dataPtr->routingInfo;
      auto it = ri.chains.find(req.chainId);
      if (it == ri.chains.end()) {
        CO_RETURN_AND_LOG_OP_ERR(*this, MgmtdCode::kChainNotFound, "chain: {}", req.chainId.toUnderType());
      }
      chainInfo = it->second;
      for (const auto &ti : chainInfo.targets) {
        auto tit = ri.getTargets().find(ti.targetId);
        XLOGF_IF(FATAL,
                 tit == ri.getTargets().end(),
                 "Target not found. chain: {}. target: {}",
                 req.chainId,
                 ti.targetId);
        oldTargets.emplace_back(tit->second.base());
      }
    }

    auto newTargets = rotateAsPreferredOrder(oldTargets, chainInfo.preferredTargetOrder);
    if (oldTargets == newTargets) {
      co_return RotateAsPreferredOrderRsp::create(std::move(chainInfo));
    }

    chainInfo.chainVersion = nextVersion(chainInfo.chainVersion);
    chainInfo.targets.clear();
    for (const auto &ti : newTargets) {
      chainInfo.targets.push_back(ti);
    }

    auto commitRes =
        co_await updateStoredRoutingInfo(state, *this, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
          co_return co_await state.store_.storeChainInfo(txn, chainInfo);
        });
    CO_RETURN_ON_ERROR(commitRes);

    co_await updateMemoryRoutingInfo(state, *this, [&](RoutingInfo &ri) {
      auto &oldChain = ri.getChain(chainInfo.chainId);
      LOG_OP_INFO(*this,
                  "{} change from {} to {}",
                  chainInfo.chainId,
                  serde::toJsonString(oldChain),
                  serde::toJsonString(chainInfo));
      oldChain = chainInfo;
      auto steadyNow = SteadyClock::now();
      for (const auto &cti : chainInfo.targets) {
        ri.updateTarget(cti.targetId, [&](auto &ti) {
          ti.base().publicState = cti.publicState;
          ti.updateTs(steadyNow);
        });
      }
    });

    co_return RotateAsPreferredOrderRsp::create(std::move(chainInfo));
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}

}  // namespace hf3fs::mgmtd
