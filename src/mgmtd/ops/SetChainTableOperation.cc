#include "SetChainTableOperation.h"

#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {
Result<Void> checkChains(core::ServiceOperation &ctx, const RoutingInfo &ri, const std::vector<flat::ChainId> &chains) {
  if (chains.empty()) {
    RETURN_AND_LOG_OP_ERR(ctx, MgmtdCode::kInvalidChainTable, "Empty chains");
  }

  size_t expectedTargets = 0;
  for (auto cid : chains) {
    auto it = ri.chains.find(cid);
    if (it == ri.chains.end()) {
      RETURN_AND_LOG_OP_ERR(ctx, MgmtdCode::kChainNotFound, "{} not found", cid);
    }
    const auto &ci = it->second;
    if (expectedTargets == 0) {
      expectedTargets = ci.targets.size();
    } else if (ci.targets.size() != expectedTargets) {
      RETURN_AND_LOG_OP_ERR(
          ctx,
          MgmtdCode::kInvalidChainTable,
          "Target count mismatch with first chain. chain id: {}. targets: {}. targets of first chain: {}.",
          cid.toUnderType(),
          ci.targets.size(),
          expectedTargets);
    }
  }
  return Void{};
}
}  // namespace

CoTryTask<SetChainTableRsp> SetChainTableOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  CO_RETURN_ON_ERROR(co_await state.validateAdmin(*this, req.user));

  auto tableId = req.chainTableId;
  auto tableVersion = flat::ChainTableVersion(1);
  auto newChainTable = flat::ChainTable::create(tableId, tableVersion, std::move(req.chains), std::move(req.desc));

  auto handler = [&]() -> CoTryTask<SetChainTableRsp> {
    auto writerLock = co_await state.coScopedLock<"SetChainTable">();

    {
      auto dataPtr = co_await state.data_.coSharedLock();

      CO_RETURN_ON_ERROR(checkChains(*this, dataPtr->routingInfo, newChainTable.chains));

      auto ctit = dataPtr->routingInfo.chainTables.find(tableId);
      if (ctit != dataPtr->routingInfo.chainTables.end()) {
        const auto &m = ctit->second;
        XLOGF_IF(FATAL, m.empty(), "{} has no versions", tableId);
        const auto &current = m.rbegin()->second;
        if (newChainTable.chains != current.chains) {
          newChainTable.chainTableVersion = nextVersion(current.chainTableVersion);
        }
        if (newChainTable.desc.empty()) {
          newChainTable.desc = current.desc;
        }
        if (newChainTable == current) {
          // same as the latest version, do not add new version
          co_return SetChainTableRsp::create(current.chainTableVersion);
        }
      }
    }

    auto commitRes =
        co_await updateStoredRoutingInfo(state, *this, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
          co_return co_await state.store_.storeChainTable(txn, newChainTable);
        });
    CO_RETURN_ON_ERROR(commitRes);

    co_await updateMemoryRoutingInfo(state, *this, [&](RoutingInfo &ri) {
      LOG_OP_INFO(*this,
                  "set ChainTable {} succeeded. new version: {}. new count: {}. desc: {}",
                  tableId.toUnderType(),
                  newChainTable.chainTableVersion.toUnderType(),
                  newChainTable.chains.size(),
                  newChainTable.desc);

      ri.chainTables[tableId][newChainTable.chainTableVersion] = newChainTable;
    });
    co_return SetChainTableRsp::create(newChainTable.chainTableVersion);
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}
}  // namespace hf3fs::mgmtd
