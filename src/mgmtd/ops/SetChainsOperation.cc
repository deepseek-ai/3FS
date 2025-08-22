#include "SetChainsOperation.h"

#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {
flat::ChainInfo makeChainInfo(const flat::ChainSetting &setting) {
  flat::ChainInfo info;
  info.chainId = setting.chainId;
  info.chainVersion = flat::ChainVersion{1};
  for (const auto &t : setting.targets) {
    flat::ChainTargetInfo cti;
    cti.targetId = t.targetId;
    cti.publicState = flat::PublicTargetState::SERVING;
    info.targets.push_back(std::move(cti));
    if (setting.setPreferredTargetOrder) {
      info.preferredTargetOrder.push_back(cti.targetId);
    }
  }
  return info;
}

Result<Void> ensureChainNotChanged(core::ServiceOperation &ctx,
                                   const flat::ChainInfo &oldChain,
                                   const flat::ChainSetting &newChain) {
  if (oldChain.chainId != newChain.chainId) {
    RETURN_AND_LOG_OP_ERR(ctx,
                          MgmtdCode::kInvalidChainTable,
                          "Chain id mismatch. old: {}. new: {}",
                          oldChain.chainId,
                          newChain.chainId);
  }
  if (oldChain.targets.size() != newChain.targets.size()) {
    RETURN_AND_LOG_OP_ERR(ctx,
                          MgmtdCode::kInvalidChainTable,
                          "Target count of chain {} mismatch. old: {}. new: {}",
                          oldChain.chainId,
                          oldChain.targets.size(),
                          newChain.targets.size());
  }
  std::vector<flat::TargetId> oldTargets(oldChain.targets.size());
  for (size_t i = 0; i < oldTargets.size(); ++i) oldTargets[i] = oldChain.targets[i].targetId;
  std::sort(oldTargets.begin(), oldTargets.end());

  std::vector<flat::TargetId> newTargets(newChain.targets.size());
  for (size_t i = 0; i < newTargets.size(); ++i) newTargets[i] = newChain.targets[i].targetId;
  std::sort(newTargets.begin(), newTargets.end());

  if (oldTargets != newTargets) {
    RETURN_AND_LOG_OP_ERR(ctx,
                          MgmtdCode::kInvalidChainTable,
                          "Target mismatch of {}. old: [{}]. new: [{}]",
                          oldChain.chainId,
                          fmt::join(oldTargets, ","),
                          fmt::join(newTargets, ","));
  }
  return Void{};
}

Result<Void> checkChains(core::ServiceOperation &ctx,
                         const RoutingInfo &ri,
                         std::span<const flat::ChainSetting> chains,
                         std::vector<flat::ChainInfo> &newChains,
                         robin_hood::unordered_set<flat::TargetId> &newTargets) {
  if (chains.empty()) {
    RETURN_AND_LOG_OP_ERR(ctx, MgmtdCode::kInvalidChainTable, "Empty chains");
  }

  for (const auto &chain : chains) {
    if (chain.chainId == 0) {
      RETURN_AND_LOG_OP_ERR(ctx, MgmtdCode::kInvalidChainTable, "Empty chain id");
    }
    if (chain.targets.empty()) {
      RETURN_AND_LOG_OP_ERR(ctx,
                            MgmtdCode::kInvalidChainTable,
                            "Chain contains no targets. chain id: {}.",
                            chain.chainId.toUnderType());
    }
    auto it = ri.chains.find(chain.chainId);
    if (it != ri.chains.end()) {
      // existed chain
      RETURN_ON_ERROR(ensureChainNotChanged(ctx, it->second, chain));
    } else {
      // new chain
      for (const auto &cti : chain.targets) {
        if (ri.getTargets().contains(cti.targetId) || !newTargets.insert(cti.targetId).second) {
          RETURN_AND_LOG_OP_ERR(ctx,
                                MgmtdCode::kInvalidChainTable,
                                "{} duplicated. chain id: {}",
                                cti.targetId,
                                chain.chainId.toUnderType());
        }
      }
      newChains.push_back(makeChainInfo(chain));
    }
  }
  return Void{};
}
}  // namespace

CoTryTask<SetChainsRsp> SetChainsOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  CO_RETURN_ON_ERROR(co_await state.validateAdmin(*this, req.user));

  const auto &chains = req.chains;

  auto handler = [&]() -> CoTryTask<SetChainsRsp> {
    auto writerLock = co_await state.coScopedLock<"SetChains">();

    std::vector<flat::ChainInfo> newChains;

    {
      auto dataPtr = co_await state.data_.coSharedLock();

      robin_hood::unordered_set<flat::TargetId> newTargets;
      CO_RETURN_ON_ERROR(
          checkChains(*this, dataPtr->routingInfo, std::span(chains.begin(), chains.size()), newChains, newTargets));
    }

    if (newChains.empty()) co_return SetChainsRsp::create();

    auto commitRes =
        co_await updateStoredRoutingInfo(state, *this, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
          for (const auto &newChain : newChains) {
            CO_RETURN_ON_ERROR(co_await state.store_.storeChainInfo(txn, newChain));
            LOG_OP_DBG(*this, "store chain {} succeeded", serde::toJsonString(newChain));
          }
          co_return Void{};
        });
    CO_RETURN_ON_ERROR(commitRes);

    LOG_OP_DBG(*this, "new chains created: {}", serde::toJsonString(newChains));

    co_await updateMemoryRoutingInfo(state, *this, [&](RoutingInfo &ri) {
      for (auto &newChain : newChains) {
        ri.insertNewChain(newChain);
      }
    });
    co_return SetChainsRsp::create();
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}
}  // namespace hf3fs::mgmtd
