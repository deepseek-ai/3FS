#include <algorithm>
#include <folly/Likely.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "client/meta/MetaClient.h"
#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "meta/components/FileHelper.h"
#include "meta/components/SessionManager.h"
#include "meta/store/FileSession.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"

namespace hf3fs::meta::server {

/** MetaStore::pruneSession */
class PruneSessionOp : public Operation<PruneSessionRsp> {
 public:
  PruneSessionOp(MetaStore &meta, const PruneSessionReq &req)
      : Operation<PruneSessionRsp>(meta),
        req_(req) {}

  OPERATION_TAGS(req_);

  CoTryTask<PruneSessionRsp> run(IReadWriteTransaction &txn) override {
    XLOGF(DBG, "PruneSessionOp::run, req {}", req_);

    CHECK_REQUEST(req_);

    static constexpr size_t kConcurrentCheck = 32;
    std::vector<CoTryTask<void>> tasks;

    auto waitRequests = [&]() -> CoTryTask<void> {
      auto results = co_await folly::coro::collectAllRange(std::exchange(tasks, {}));
      for (auto &rsp : results) {
        CO_RETURN_ON_ERROR(rsp);
      }
      co_return Void{};
    };

    for (size_t i = 0; i < req_.sessions.size(); i++) {
      auto sessionId = req_.sessions[i];
      tasks.push_back(prune(txn, sessionId));
      if (tasks.size() == kConcurrentCheck || i + 1 >= req_.sessions.size()) {
        CO_RETURN_ON_ERROR(co_await waitRequests());
      }
    }

    co_return PruneSessionRsp();
  }

 private:
  CoTryTask<void> prune(IReadWriteTransaction &txn, const Uuid sessionId) {
    auto session = FileSession::createPrune(req_.client, sessionId);
    CO_RETURN_ON_ERROR(co_await session.store(txn));
    co_return Void{};
  }

  const PruneSessionReq &req_;
};

MetaStore::OpPtr<PruneSessionRsp> MetaStore::pruneSession(const PruneSessionReq &req) {
  return std::make_unique<PruneSessionOp>(*this, req);
}
}  // namespace hf3fs::meta::server
