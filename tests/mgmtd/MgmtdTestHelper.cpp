#include "MgmtdTestHelper.h"

namespace hf3fs::mgmtd::testing {
CoTryTask<void> MgmtdTestHelper::setRoutingInfo(std::function<void(flat::RoutingInfo &)> callback) {
  co_await extendLease();

  auto handler = [&]() -> CoTryTask<void> {
    auto writerLock = co_await op_.state_.coScopedLock<"test">();
    std::optional<flat::RoutingInfo> tmpRoutingInfo;
    {
      auto dataPtr = co_await op_.state_.data_.coSharedLock();

      tmpRoutingInfo = dataPtr->getRoutingInfo(flat::RoutingInfoVersion(0), op_.state_.config_);
      XLOGF_IF(FATAL, !tmpRoutingInfo, "Empty routingInfo!");
      CO_RETURN_ON_ERROR(checkDataIntegrity(*tmpRoutingInfo));

      auto rv = tmpRoutingInfo->routingInfoVersion;
      callback(*tmpRoutingInfo);

      CO_RETURN_ON_ERROR(checkDataIntegrity(*tmpRoutingInfo));
      tmpRoutingInfo->routingInfoVersion =
          std::max(tmpRoutingInfo->routingInfoVersion, flat::RoutingInfoVersion(rv + 1));
    }

    auto storeHandler = [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
      CO_RETURN_ON_ERROR(co_await op_.state_.store_.storeRoutingInfo(txn, *tmpRoutingInfo));
      co_return Void{};
    };
    CO_RETURN_ON_ERROR(co_await kv::WithTransaction(op_.state_.createRetryStrategy())
                           .run(op_.state_.env_->kvEngine()->createReadWriteTransaction(), std::move(storeHandler)));

    {
      auto dataPtr = co_await op_.state_.data_.coLock();
      auto &ri = dataPtr->routingInfo;

      ri.nodeMap.clear();
      ri.chainTables.clear();
      ri.targets.clear();
      ri.newBornChains.clear();

      ++ri.routingInfoVersion.toUnderType();
      for ([[maybe_unused]] const auto &[nodeId, node] : tmpRoutingInfo->nodes) {
        ri.nodeMap.try_emplace(nodeId, node);
      }
      ri.chainTables = tmpRoutingInfo->chainTables;
      ri.chains = tmpRoutingInfo->chains;
      for ([[maybe_unused]] const auto &[tid, ti] : tmpRoutingInfo->targets) {
        ri.targets[tid].base() = ti;
      }
    }
    co_return Void{};
  };

  co_return co_await doAsPrimary(op_.state_, std::move(handler));
}

Result<Void> MgmtdTestHelper::checkDataIntegrity(const flat::RoutingInfo &ri) {
  using PS = flat::PublicTargetState;
  using LS = flat::LocalTargetState;
  // check all nodes valid
  {
    for ([[maybe_unused]] const auto &[nodeId, node] : ri.nodes) {
      if (nodeId == 0) {
        return makeError(StatusCode::kDataCorruption, "NodeId is 0");
      }
    }
  }
  // check all chaintables
  {
    for (const auto &[chainTableId, chainTables] : ri.chainTables) {
      if (chainTableId == 0) {
        return makeError(StatusCode::kDataCorruption, "ChainTableId is 0");
      }
      if (chainTables.empty()) {
        return makeError(StatusCode::kDataCorruption, fmt::format("Empty versions of {}", chainTableId));
      }
      for (const auto &[v, table] : chainTables) {
        if (v == 0) {
          return makeError(StatusCode::kDataCorruption, fmt::format("ChainTableVersion of {} is 0", chainTableId));
        }
        if (table.chains.empty()) {
          return makeError(StatusCode::kDataCorruption, fmt::format("Empty chains of {} with {}", chainTableId, v));
        }
        robin_hood::unordered_set<flat::ChainId> s;
        size_t targets = 0;
        for (auto cid : table.chains) {
          if (cid == 0) {
            return makeError(StatusCode::kDataCorruption,
                             fmt::format("ChainId(0) found in {} with {}", chainTableId, v));
          }
          if (!ri.chains.contains(cid)) {
            return makeError(StatusCode::kDataCorruption,
                             fmt::format("{} in {} with {} not existed", cid, chainTableId, v));
          }
          if (!s.insert(cid).second) {
            return makeError(StatusCode::kDataCorruption,
                             fmt::format("{} duplicated in {} with {}", cid, chainTableId, v));
          }
          const auto &chain = ri.chains.at(cid);
          if (targets == 0) {
            targets = chain.targets.size();
          } else if (targets != chain.targets.size()) {
            return makeError(StatusCode::kDataCorruption,
                             fmt::format("Multiple replica count found in {} with {}", chainTableId, v));
          }
        }
      }
    }
  }
  // check all chains
  for ([[maybe_unused]] const auto &[_, chain] : ri.chains) {
    if (chain.chainId == 0) {
      return makeError(StatusCode::kDataCorruption, "ChainId is 0");
    }
    if (chain.targets.empty()) {
      return makeError(StatusCode::kDataCorruption, fmt::format("Empty targets of {}", chain.chainId));
    }
    auto lastPs = PS::INVALID;
    for (const auto &cti : chain.targets) {
      if (cti.targetId == 0) {
        return makeError(StatusCode::kDataCorruption, fmt::format("TargetId of {} is 0", chain.chainId));
      }
      if (cti.publicState == flat::PublicTargetState::INVALID) {
        return makeError(StatusCode::kDataCorruption,
                         fmt::format("PublicTargetState of {} of {} is INVALID", cti.targetId, chain.chainId));
      }
      if (cti.publicState < lastPs) {
        return makeError(
            StatusCode::kDataCorruption,
            fmt::format("PublicTargetState order inversion: {} after {}", toString(cti.publicState), toString(lastPs)));
      }
      lastPs = cti.publicState;
    }
  }

  // check all targets valid
  {
    for (const auto &[tid, ti] : ri.targets) {
      if (tid == 0) {
        return makeError(StatusCode::kDataCorruption, "TargetId is 0");
      }
      if (ti.publicState == PS::INVALID) {
        return makeError(StatusCode::kDataCorruption, fmt::format("PublicTargetState of {} is INVALID", tid));
      }
      if (ti.localState == LS::INVALID) {
        return makeError(StatusCode::kDataCorruption, fmt::format("LocalTargetState of {} is INVALID", tid));
      }
      if (ti.chainId == 0) {
        return makeError(StatusCode::kDataCorruption, fmt::format("ChainId of {} is 0", tid));
      }

      if (!ri.getChain(ti.chainId)) {
        return makeError(StatusCode::kDataCorruption, fmt::format("ChainId {} of {} not found", ti.chainId, tid));
      }

      if (ti.nodeId && !ri.getNode(*ti.nodeId)) {
        return makeError(StatusCode::kDataCorruption, fmt::format("NodeId {} of {} not found", *ti.nodeId, tid));
      }
    }
  }
  return Void{};
}
}  // namespace hf3fs::mgmtd::testing
