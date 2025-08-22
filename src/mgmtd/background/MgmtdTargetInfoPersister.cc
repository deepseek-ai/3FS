#include "MgmtdTargetInfoPersister.h"

#include "common/utils/OptionalUtils.h"
#include "core/utils/ServiceOperation.h"
#include "core/utils/runOp.h"
#include "mgmtd/service/MgmtdState.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {

#define OP_NAME "PersistTargetInfo"

struct Op : core::ServiceOperationWithMetric<"MgmtdService", OP_NAME, "bg"> {
  String toStringImpl() const final { return OP_NAME; }

  auto handle(MgmtdState &state) -> CoTryTask<void> {
    std::vector<flat::TargetInfo> candidates;
    {
      auto dataPtr = co_await state.data_.coSharedLock();
      const auto &ri = dataPtr->routingInfo;
      for (const auto &[tid, ti] : ri.getTargets()) {
        if (ti.locationInitLoaded  // the persisted location is known
            && ti.base().nodeId    // the actual location is known
            && (ti.persistedNodeId != ti.base().nodeId ||
                ti.persistedDiskIndex != ti.base().diskIndex)  // and they are different
        ) {
          candidates.push_back(ti.base());
          if (static_cast<int>(candidates.size()) >= state.config_.target_info_persist_batch()) break;
        }
      }
    }

    if (candidates.empty()) {
      co_return Void{};
    }

    auto handler = [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
      for (const auto &ti : candidates) {
        CO_RETURN_ON_ERROR(co_await state.store_.storeTargetInfo(txn, ti));
      }
      co_return Void{};
    };

    auto res = co_await withReadWriteTxn(state, std::move(handler));
    CO_RETURN_ON_ERROR(res);

    {
      auto dataPtr = co_await state.data_.coLock();
      for (const auto &persisted : candidates) {
        dataPtr->routingInfo.updateTarget(persisted.targetId, [&](auto &ti) {
          ti.persistedNodeId = persisted.nodeId;
          ti.persistedDiskIndex = persisted.diskIndex;
          LOG_OP_DBG(*this,
                     "TargetInfo of {} persisted, nodeId={}, diskIndex={}",
                     persisted.targetId,
                     persisted.nodeId,
                     OptionalFmt(persisted.diskIndex));
        });
      }
    }

    co_return Void{};
  }
};
}  // namespace
MgmtdTargetInfoPersister::MgmtdTargetInfoPersister(MgmtdState &state)
    : state_(state) {}

CoTask<void> MgmtdTargetInfoPersister::run() {
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
