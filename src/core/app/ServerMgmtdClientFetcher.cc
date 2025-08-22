#include "ServerMgmtdClientFetcher.h"

#include <folly/experimental/coro/BlockingWait.h>

namespace hf3fs::core::launcher {
Result<Void> ServerMgmtdClientFetcher::completeAppInfo(flat::AppInfo &appInfo) {
  RETURN_ON_ERROR(ensureClientInited());
  return folly::coro::blockingWait([&]() -> CoTryTask<void> {
    auto routingInfo = mgmtdClient_->getRoutingInfo();
    if (!routingInfo || !routingInfo->raw()) co_return makeError(MgmtdClientCode::kRoutingInfoNotReady);
    const auto *ni = routingInfo->raw()->getNode(appInfo.nodeId);
    if (ni) {
      appInfo.tags = ni->tags;
    }
    co_return Void{};
  }());
}
}  // namespace hf3fs::core::launcher
