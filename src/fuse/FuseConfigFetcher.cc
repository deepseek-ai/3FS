#include "FuseConfigFetcher.h"

#include <folly/experimental/coro/BlockingWait.h>

#include "common/utils/SysResource.h"

namespace hf3fs::fuse {
Result<Void> FuseConfigFetcher::completeAppInfo(flat::AppInfo &appInfo [[maybe_unused]]) {
  auto hostnameRes = SysResource::hostname(/*physicalMachineName=*/true);
  RETURN_ON_ERROR(hostnameRes);
  RETURN_ON_ERROR(ensureClientInited());
  return folly::coro::blockingWait([&]() -> CoTryTask<void> {
    auto tagsRes = co_await mgmtdClient_->getUniversalTags(*hostnameRes);
    CO_RETURN_ON_ERROR(tagsRes);
    appInfo.tags = std::move(*tagsRes);
    co_return Void{};
  }());
}
}  // namespace hf3fs::fuse
