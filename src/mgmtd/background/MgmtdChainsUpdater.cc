#include "MgmtdChainsUpdater.h"

#include "core/utils/ServiceOperation.h"
#include "core/utils/runOp.h"
#include "mgmtd/service/MgmtdState.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {
struct Op : core::ServiceOperationWithMetric<"MgmtdService", "UpdateChains", "bg"> {
  String toStringImpl() const final { return "UpdateChains"; }

  CoTryTask<SteadyTime> handle(MgmtdState &state, SteadyTime lastUpdateTs, bool lastAdjustTargetOrderFlag) {
    auto writerLock = co_await state.coScopedLock<"UpdateChains">();

    robin_hood::unordered_set<flat::ChainId> candidateChains;
    std::vector<flat::ChainInfo> changedChains;
    bool needPromoteRoutingInfoVersion = false;

    auto steadyNow = SteadyClock::now();
    {
      auto dataPtr = co_await state.data_.coSharedLock();
      const auto &ri = dataPtr->routingInfo;

      for (const auto &[tid, ti] : ri.getTargets()) {
        if (ti.ts() > lastUpdateTs) {
          candidateChains.insert(ti.base().chainId);
        } else if (!lastAdjustTargetOrderFlag && state.config_.try_adjust_target_order_as_preferred()) {
          candidateChains.insert(ti.base().chainId);
        }
        needPromoteRoutingInfoVersion |= ti.importantInfoChangedTime > lastUpdateTs;
      }

      for (auto chainId : candidateChains) {
        dataPtr->appendChangedChains(chainId, changedChains, state.config_.try_adjust_target_order_as_preferred());
      }
    }

    if (changedChains.empty() && !needPromoteRoutingInfoVersion) co_return steadyNow;

    auto commitRes =
        co_await updateStoredRoutingInfo(state, *this, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
          for (const auto &chain : changedChains) {
            CO_RETURN_ON_ERROR(co_await state.store_.storeChainInfo(txn, chain));
          }
          co_return Void{};
        });
    CO_RETURN_ON_ERROR(commitRes);

    co_await updateMemoryRoutingInfo(state, *this, [&](RoutingInfo &ri) {
      ri.applyChainTargetChanges(*this, changedChains, steadyNow);
    });

    co_return steadyNow;
  }
};
}  // namespace

MgmtdChainsUpdater::MgmtdChainsUpdater(MgmtdState &state)
    : state_(state) {}

CoTask<void> MgmtdChainsUpdater::update(SteadyTime &lastUpdateTs) {
  Op op;

  auto handler = [&]() -> CoTryTask<SteadyTime> {
    CO_INVOKE_OP_INFO(op, "background", state_, lastUpdateTs, lastAdjustTargetOrderFlag);
  };

  auto res = co_await doAsPrimary(state_, std::move(handler));
  if (res.hasError()) {
    if (res.error().code() == MgmtdCode::kNotPrimary)
      XLOGF(INFO, "Mgmtd: self is not primary, skip updateChains");
    else
      LOG_OP_ERR(op, "failed: {}", res.error());
  } else {
    lastUpdateTs = *res;
    lastAdjustTargetOrderFlag = state_.config_.try_adjust_target_order_as_preferred();
  }
}
}  // namespace hf3fs::mgmtd
