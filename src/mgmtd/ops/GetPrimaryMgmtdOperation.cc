#include "GetPrimaryMgmtdOperation.h"

#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<GetPrimaryMgmtdRsp> GetPrimaryMgmtdOperation::handle(MgmtdState &state) const {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  auto now = state.utcNow();
  auto lease = co_await state.currentLease(now);
  if (lease.has_value()) {
    co_return GetPrimaryMgmtdRsp::create(std::move(lease->primary));
  }
  auto handler = [&](kv::IReadOnlyTransaction &txn) -> CoTryTask<std::optional<flat::MgmtdLeaseInfo>> {
    co_return co_await state.store_.loadMgmtdLeaseInfo(txn);
  };
  auto res = co_await kv::WithTransaction(state.createRetryStrategy())
                 .run(state.env_->kvEngine()->createReadonlyTransaction(), std::move(handler));
  CO_RETURN_ON_ERROR(res);
  if (res->has_value()) {
    co_return GetPrimaryMgmtdRsp::create(std::move(res->value().primary));
  }
  co_return GetPrimaryMgmtdRsp::create(std::nullopt);
}
}  // namespace hf3fs::mgmtd
