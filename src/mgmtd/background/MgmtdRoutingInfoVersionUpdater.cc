#include "MgmtdRoutingInfoVersionUpdater.h"

#include "core/utils/ServiceOperation.h"
#include "core/utils/runOp.h"
#include "mgmtd/service/MgmtdState.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {
struct Op : core::ServiceOperationWithMetric<"MgmtdService", "BumpRoutingInfoVersion", "bg"> {
  String toStringImpl() const final { return "BumpRoutingInfoVersion"; }

  auto handle(MgmtdState &state) -> CoTryTask<void> {
    auto writerLock = co_await state.coScopedLock<"BumpRoutingInfoVersion">();
    bool needChange = co_await [&]() -> CoTask<bool> {
      auto dataPtr = co_await state.data_.coSharedLock();
      co_return dataPtr->routingInfo.routingInfoChanged;
    }();

    if (needChange) {
      CO_RETURN_ON_ERROR(co_await updateStoredRoutingInfo(state, *this));
      co_await updateMemoryRoutingInfo(state, *this);
    }

    co_return Void{};
  }
};
}  // namespace
MgmtdRoutingInfoVersionUpdater::MgmtdRoutingInfoVersionUpdater(MgmtdState &state)
    : state_(state) {}

CoTask<void> MgmtdRoutingInfoVersionUpdater::update() {
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
