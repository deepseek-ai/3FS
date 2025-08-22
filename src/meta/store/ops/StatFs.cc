#include <chrono>
#include <folly/logging/xlog.h>
#include <memory>

#include "common/kv/ITransaction.h"
#include "common/utils/Result.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"

namespace hf3fs::meta::server {

/** MetaStore::statFs */
class StatFsOp : public ReadOnlyOperation<StatFsRsp> {
 public:
  StatFsOp(MetaStore &meta, const StatFsReq &req)
      : ReadOnlyOperation<StatFsRsp>(meta),
        req_(req) {}

  OPERATION_TAGS(req_);

  CoTryTask<StatFsRsp> run(IReadOnlyTransaction &) override {
    XLOGF(DBG, "StatFsOp::run {}", req_);
    co_return co_await fileHelper().statFs(req_.user, std::chrono::seconds(30));
  }

 private:
  const StatFsReq &req_;
};

MetaStore::OpPtr<StatFsRsp> MetaStore::statFs(const StatFsReq &req) { return std::make_unique<StatFsOp>(*this, req); }

}  // namespace hf3fs::meta::server
