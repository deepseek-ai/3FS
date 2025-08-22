#include "MgmtdTargetInfoLoader.h"

#include "common/utils/OptionalUtils.h"
#include "core/utils/ServiceOperation.h"
#include "core/utils/runOp.h"
#include "mgmtd/service/MgmtdState.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {

#define OP_NAME "LoadTargetInfo"

struct Op : core::ServiceOperationWithMetric<"MgmtdService", OP_NAME, "bg"> {
  Op(SteadyTime &loadedLeaseStart)
      : loadedLeaseStart_(loadedLeaseStart) {}

  String toStringImpl() const final { return OP_NAME; }

  auto handle(MgmtdState &state) -> CoTryTask<void> {
    {
      if ((co_await ensureSelfIsPrimary(state)).hasError()) {
        co_return Void{};
      }

      auto dataPtr = co_await state.data_.coSharedLock();
      if (loadedLeaseStart_ >= dataPtr->leaseStartTs) {
        co_return Void{};
      }

      loadedLeaseStart_ = dataPtr->leaseStartTs;
    }

    flat::TargetId startTid(0);
    for (;;) {
      auto handler = [&](kv::IReadOnlyTransaction &txn) -> CoTryTask<std::vector<flat::TargetInfo>> {
        return state.store_.loadTargetsFrom(txn, startTid);
      };

      auto res = co_await withReadOnlyTxn(state, std::move(handler));
      CO_RETURN_ON_ERROR(res);

      LOG_OP_DBG(*this, "load {} targets starting from {}", res->size(), startTid);

      if (res->empty()) break;
      startTid = flat::TargetId(res->back().targetId + 1);

      if ((co_await ensureSelfIsPrimary(state)).hasError()) {
        co_return Void{};
      }

      auto dataPtr = co_await state.data_.coLock();
      if (loadedLeaseStart_ < dataPtr->leaseStartTs) {
        co_return Void{};
      }

      for (auto &loaded : *res) {
        auto tid = loaded.targetId;
        auto updater = [this, loaded = std::move(loaded)](TargetInfo &ti) {
          ti.locationInitLoaded = true;
          ti.persistedNodeId = loaded.nodeId;
          ti.persistedDiskIndex = loaded.diskIndex;
          LOG_OP_DBG(*this,
                     "TargetInfo of {} loaded, nodeId={}, diskIndex={}",
                     loaded.targetId,
                     loaded.nodeId,
                     OptionalFmt(loaded.diskIndex));
          if (!ti.base().nodeId) {
            ti.base().nodeId = loaded.nodeId;
            ti.base().diskIndex = loaded.diskIndex;
            LOG_OP_DBG(*this, "Fill TargetInfo of {}", loaded.targetId);
          }
        };
        dataPtr->routingInfo.updateTarget(tid, std::move(updater));
      }
    }

    if ((co_await ensureSelfIsPrimary(state)).hasError()) {
      co_return Void{};
    }

    {
      auto dataPtr = co_await state.data_.coLock();
      if (loadedLeaseStart_ < dataPtr->leaseStartTs) {
        co_return Void{};
      }

      for (const auto &[tid, _] : dataPtr->routingInfo.getTargets()) {
        auto updater = [this](TargetInfo &ti) {
          if (!ti.locationInitLoaded) {
            ti.locationInitLoaded = true;
            LOG_OP_DBG(*this, "TargetInfo of {} not found in kv", ti.base().targetId);
          }
        };
        dataPtr->routingInfo.updateTarget(tid, std::move(updater));
      }
    }

    co_return Void{};
  }

  SteadyTime &loadedLeaseStart_;
};
}  // namespace
MgmtdTargetInfoLoader::MgmtdTargetInfoLoader(MgmtdState &state)
    : state_(state) {}

CoTask<void> MgmtdTargetInfoLoader::run() {
  Op op(loadedLeaseStart);
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
