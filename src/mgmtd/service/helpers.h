#include <atomic>
#include <folly/experimental/coro/Sleep.h>

#include "MgmtdConfig.h"
#include "MgmtdOperator.h"
#include "MgmtdState.h"
#include "common/kv/WithTransaction.h"
#include "core/utils/ServiceOperation.h"

#define RECORD_LATENCY(latency)                                                                                       \
  auto FB_ANONYMOUS_VARIABLE(guard) = folly::makeGuard([lat = &(latency), begin = std::chrono::steady_clock::now()] { \
    lat->addSample(std::chrono::steady_clock::now() - begin);                                                         \
  })

namespace hf3fs::mgmtd {
template <typename T>
inline T nextVersion(T version) {
  auto v = version + 1;
  return T(v);
}

CoTryTask<Void> ensureSelfIsPrimary(MgmtdState &state);

template <typename Handler>
inline std::invoke_result_t<Handler> doAsPrimary(MgmtdState &state, Handler &&handler) {
  auto ret = co_await ensureSelfIsPrimary(state);
  CO_RETURN_ON_ERROR(ret);
  co_return co_await handler();
}

void updateMemoryRoutingInfo(RoutingInfo &alreadyLockedRoutingInfo, core::ServiceOperation &ctx);

template <typename Handler>
inline CoTask<void> updateMemoryRoutingInfo(MgmtdState &state, core::ServiceOperation &ctx, Handler &&handler) {
  auto dataPtr = co_await state.data_.coLock();
  auto &ri = dataPtr->routingInfo;
  updateMemoryRoutingInfo(ri, ctx);
  handler(ri);
}

CoTask<void> updateMemoryRoutingInfo(MgmtdState &state, core::ServiceOperation &ctx);

template <typename Handler, typename Result = std::invoke_result_t<Handler, kv::IReadWriteTransaction &>>
inline Result withReadWriteTxn(MgmtdState &state, Handler &&handler, bool expectSelfPrimary = true) {
  int maxRetryTimes = state.config_.retry_times_on_txn_errors();
  constexpr auto retryInterval = std::chrono::milliseconds(1000);
  auto strategy = state.createRetryStrategy();
  for (int i = 0;; ++i) {
    auto res = co_await kv::WithTransaction(strategy).run(
        state.env_->kvEngine()->createReadWriteTransaction(),
        [&](kv::IReadWriteTransaction &txn) -> Result {
          if (expectSelfPrimary && state.config_.validate_lease_on_write()) {
            CO_RETURN_ON_ERROR(co_await state.store_.ensureLeaseValid(txn, state.selfId(), state.utcNow()));
          }
          co_return co_await handler(txn);
        });
    if (res || i == maxRetryTimes || StatusCode::typeOf(res.error().code()) != StatusCodeType::Transaction)
      co_return res;
    XLOGF(CRITICAL, "Transaction failed: {}\nretryCount: {}", res.error(), i);
    co_await folly::coro::sleep(retryInterval);
  }
}

template <typename Handler, typename Result = std::invoke_result_t<Handler, kv::IReadOnlyTransaction &>>
inline Result withReadOnlyTxn(MgmtdState &state, Handler &&handler) {
  auto strategy = state.createRetryStrategy();
  co_return co_await kv::WithTransaction(strategy).run(
      state.env_->kvEngine()->createReadonlyTransaction(),
      [&](kv::IReadOnlyTransaction &txn) -> Result { co_return co_await handler(txn); });
}

template <typename Handler>
inline CoTryTask<void> updateStoredRoutingInfo(MgmtdState &state, core::ServiceOperation &ctx, Handler &&handler) {
  auto dataPtr = co_await state.data_.coSharedLock();
  auto nextv = nextVersion(dataPtr->routingInfo.routingInfoVersion);
  co_return co_await withReadWriteTxn(state, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
    CO_RETURN_ON_ERROR(co_await state.store_.storeRoutingInfoVersion(txn, nextv));
    LOG_OP_INFO(ctx, "RoutingInfo: bump storage version to {}", nextv);
    co_return co_await handler(txn);
  });
}

CoTryTask<void> updateStoredRoutingInfo(MgmtdState &state, core::ServiceOperation &ctx);

flat::TargetInfo makeTargetInfo(flat::ChainId chainId, const flat::ChainTargetInfo &info);

Result<Void> updateSelfConfig(MgmtdState &state, const flat::ConfigInfo &cfg);

using MgmtdStub = mgmtd::IMgmtdServiceStub;
using MgmtdStubFactory = stubs::IStubFactory<MgmtdStub>;

Result<std::vector<flat::TagPair>> updateTags(core::ServiceOperation &op,
                                              flat::SetTagMode mode,
                                              const std::vector<flat::TagPair> &oldTags,
                                              const std::vector<flat::TagPair> &updates);
}  // namespace hf3fs::mgmtd
