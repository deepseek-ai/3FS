#include "SetPreferredTargetOrderOperation.h"

#include "common/utils/StringUtils.h"
#include "mgmtd/service/helpers.h"
#include "mgmtd/service/updateChain.h"

namespace hf3fs::mgmtd {
CoTryTask<SetPreferredTargetOrderRsp> SetPreferredTargetOrderOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  CO_RETURN_ON_ERROR(co_await state.validateAdmin(*this, req.user));

  if (req.chainId == 0) {
    CO_RETURN_AND_LOG_OP_ERR(*this, StatusCode::kInvalidArg, "Empty chain id");
  }

  if (req.preferredTargetOrder.empty()) {
    CO_RETURN_AND_LOG_OP_ERR(*this, StatusCode::kInvalidArg, "Empty preferredTargetOrder");
  }

  std::set<flat::TargetId> uniqueIds;
  for (auto id : req.preferredTargetOrder) {
    uniqueIds.insert(id);
  }

  if (uniqueIds.size() != req.preferredTargetOrder.size()) {
    CO_RETURN_AND_LOG_OP_ERR(*this, StatusCode::kInvalidArg, "Found duplicated target id in preferredTargetOrder");
  }

  auto handler = [&]() -> CoTryTask<SetPreferredTargetOrderRsp> {
    auto writerLock = co_await state.coScopedLock<"SetPreferredTargetOrder">();

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

    for (auto id : uniqueIds) {
      if (!std::any_of(oldTargets.begin(), oldTargets.end(), [id](const ChainTargetInfoEx &info) {
            return info.targetId == id;
          })) {
        CO_RETURN_AND_LOG_OP_ERR(*this, StatusCode::kInvalidArg, "{} not found in chain", id);
      }
    }

    if (chainInfo.preferredTargetOrder == req.preferredTargetOrder) {
      co_return SetPreferredTargetOrderRsp::create(std::move(chainInfo));
    }

    chainInfo.preferredTargetOrder = req.preferredTargetOrder;

    // do not increase RoutingInfoVersion
    auto commitRes = co_await withReadWriteTxn(state, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
      co_return co_await state.store_.storeChainInfo(txn, chainInfo);
    });
    CO_RETURN_ON_ERROR(commitRes);

    {
      auto dataPtr = co_await state.data_.coLock();
      auto &oldChain = dataPtr->routingInfo.getChain(chainInfo.chainId);
      LOG_OP_INFO(*this,
                  "{} change preferredTargetOrder from {} to {}",
                  chainInfo.chainId,
                  serde::toJsonString(oldChain.preferredTargetOrder),
                  serde::toJsonString(chainInfo.preferredTargetOrder));
      oldChain = chainInfo;
      auto steadyNow = SteadyClock::now();
      for (auto tid : req.preferredTargetOrder) {
        dataPtr->routingInfo.updateTarget(tid, [&](auto &ti) { ti.updateTs(steadyNow); });
      }
    }

    co_return SetPreferredTargetOrderRsp::create(std::move(chainInfo));
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}

}  // namespace hf3fs::mgmtd
