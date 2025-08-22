#include "UpdateChainOperation.h"

#include "common/utils/StringUtils.h"
#include "mgmtd/service/helpers.h"
#include "mgmtd/service/updateChain.h"

namespace hf3fs::mgmtd {
CoTryTask<UpdateChainRsp> UpdateChainOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  CO_RETURN_ON_ERROR(co_await state.validateAdmin(*this, req.user));

  if (req.chainId == 0) {
    CO_RETURN_AND_LOG_OP_ERR(*this, StatusCode::kInvalidArg, "Empty chain id");
  }

  if (req.targetId == 0) {
    CO_RETURN_AND_LOG_OP_ERR(*this, StatusCode::kInvalidArg, "Empty target id");
  }

  if (req.mode != UpdateChainReq::Mode::ADD && req.mode != UpdateChainReq::Mode::REMOVE) {
    CO_RETURN_AND_LOG_OP_ERR(*this,
                             StatusCode::kInvalidArg,
                             "Unsupported mode: {}({})",
                             hf3fs::toStringView(req.mode),
                             static_cast<int>(req.mode));
  }
  bool addTarget = req.mode == UpdateChainReq::Mode::ADD;

  auto handler = [&]() -> CoTryTask<UpdateChainRsp> {
    auto writerLock = co_await state.coScopedLock<"UpdateChain">();

    flat::ChainInfo chainInfo;
    {
      auto dataPtr = co_await state.data_.coLock();
      auto &ri = dataPtr->routingInfo;
      auto it = ri.chains.find(req.chainId);
      if (it == ri.chains.end()) {
        CO_RETURN_AND_LOG_OP_ERR(*this, MgmtdCode::kChainNotFound, "chain: {}", req.chainId.toUnderType());
      }
      auto tit = ri.getTargets().find(req.targetId);
      if (req.mode == UpdateChainReq::Mode::ADD && tit != ri.getTargets().end()) {
        CO_RETURN_AND_LOG_OP_ERR(*this, MgmtdCode::kTargetExisted, "target: {}", req.targetId.toUnderType());
      }
      chainInfo = it->second;
    }

    if (addTarget) {
      // already checked req.targetId is not present in any chain,
      // directly append it into this chain
      flat::ChainTargetInfo cti;
      cti.targetId = req.targetId;
      cti.publicState = flat::PublicTargetState::OFFLINE;

      chainInfo.chainVersion = nextVersion(chainInfo.chainVersion);
      chainInfo.targets.push_back(cti);
      if (chainInfo.preferredTargetOrder.size() == chainInfo.targets.size() - 1) {
        chainInfo.preferredTargetOrder.push_back(req.targetId);
      }
    } else {
      XLOGF_IF(DFATAL, req.mode != UpdateChainReq::Mode::REMOVE, "Invalid mode: {}", static_cast<int>(req.mode));
      auto tit = std::find_if(chainInfo.targets.begin(), chainInfo.targets.end(), [&](const auto &cti) {
        return cti.targetId == req.targetId;
      });
      if (tit == chainInfo.targets.end()) {
        CO_RETURN_AND_LOG_OP_ERR(*this,
                                 MgmtdCode::kTargetNotFound,
                                 "target {} is not found in chain {}",
                                 req.targetId.toUnderType(),
                                 req.chainId.toUnderType());
      }
      if (tit->publicState != flat::PublicTargetState::OFFLINE) {
        CO_RETURN_AND_LOG_OP_ERR(*this,
                                 StatusCode::kInvalidArg,
                                 "Do not allow to remove target {} from chain {}: state is {}",
                                 req.targetId.toUnderType(),
                                 req.chainId.toUnderType(),
                                 hf3fs::toStringView(tit->publicState));
      }

      chainInfo.chainVersion = nextVersion(chainInfo.chainVersion);
      chainInfo.targets.erase(tit);
      if (auto it =
              std::find(chainInfo.preferredTargetOrder.begin(), chainInfo.preferredTargetOrder.end(), req.targetId);
          it != chainInfo.preferredTargetOrder.end()) {
        chainInfo.preferredTargetOrder.erase(it);
      }
    }

    auto commitRes =
        co_await updateStoredRoutingInfo(state, *this, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
          if (!addTarget) {
            CO_RETURN_ON_ERROR(co_await state.store_.clearTargetInfo(txn, req.targetId));
          }
          co_return co_await state.store_.storeChainInfo(txn, chainInfo);
        });
    CO_RETURN_ON_ERROR(commitRes);

    co_await updateMemoryRoutingInfo(state, *this, [&](RoutingInfo &ri) {
      auto &oldChain = ri.getChain(req.chainId);
      LOG_OP_INFO(*this,
                  "{} change from {} to {}",
                  chainInfo.chainId,
                  serde::toJsonString(oldChain),
                  serde::toJsonString(chainInfo));
      oldChain = chainInfo;
      if (addTarget) {
        XLOGF_IF(DFATAL,
                 req.targetId != chainInfo.targets.back().targetId,
                 "targetId mismatch: {} and {}",
                 req.targetId.toUnderType(),
                 chainInfo.targets.back().targetId.toUnderType());
        ri.insertNewTarget(req.chainId, chainInfo.targets.back());
      } else {
        ri.removeTarget(req.chainId, req.targetId);
      }
    });

    co_return UpdateChainRsp::create(std::move(chainInfo));
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}

}  // namespace hf3fs::mgmtd
