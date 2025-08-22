#include "MgmtdNewBornChainsChecker.h"

#include "core/utils/ServiceOperation.h"
#include "core/utils/runOp.h"
#include "mgmtd/service/MgmtdState.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {
struct Op : core::ServiceOperationWithMetric<"MgmtdService", "CheckNewBornChains", "bg"> {
  String toStringImpl() const final { return "CheckNewBornChains"; }

  auto handle(MgmtdState &state) -> CoTryTask<void> {
    auto bootstrapInterval = state.config_.new_chain_bootstrap_interval().asUs();

    SteadyTime leaseStartTs;
    std::vector<flat::ChainId> candidates;
    auto steadyNow = SteadyClock::now();
    {
      auto dataPtr = co_await state.data_.coSharedLock();
      leaseStartTs = dataPtr->leaseStartTs;

      const auto &ri = dataPtr->routingInfo;

      for (const auto &[chainId, bornTime] : ri.newBornChains) {
        if (bornTime + bootstrapInterval <= steadyNow) {
          candidates.push_back(chainId);
        }
      }
    }

    if (!candidates.empty()) {
      LOG_OP_DBG(*this, "candidates {}", fmt::join(candidates, ","));
      auto dataPtr = co_await state.data_.coLock();
      if (leaseStartTs != dataPtr->leaseStartTs) {
        LOG_OP_DBG(*this, "lease changed, skip this round");
        co_return Void{};
      }

      auto steadyNow = SteadyClock::now();
      auto &ri = dataPtr->routingInfo;
      for (auto cid : candidates) {
        ri.newBornChains.erase(cid);
        const auto &chain = dataPtr->routingInfo.getChain(cid);
        for (const auto &cti : chain.targets) {
          dataPtr->routingInfo.updateTarget(cti.targetId, [&](auto &ti) { ti.updateTs(steadyNow); });
        }
      }
    }
    co_return Void{};
  }
};
}  // namespace

MgmtdNewBornChainsChecker::MgmtdNewBornChainsChecker(MgmtdState &state)
    : state_(state) {}

CoTask<void> MgmtdNewBornChainsChecker::check() {
  Op op;
  auto handler = [&]() -> CoTryTask<void> { CO_INVOKE_OP_INFO(op, "background", state_); };

  auto res = co_await doAsPrimary(state_, std::move(handler));
  if (res.hasError()) {
    if (res.error().code() == MgmtdCode::kNotPrimary)
      LOG_OP_INFO(op, "self is not primary, skip");
    else
      LOG_OP_ERR(op, "failed: {}", res.error());
  }
}

}  // namespace hf3fs::mgmtd
