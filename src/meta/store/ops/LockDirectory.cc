#include <folly/logging/xlog.h>
#include <memory>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"
#include "meta/store/Utils.h"

namespace hf3fs::meta::server {
class LockDirectoryOp : public Operation<LockDirectoryRsp> {
 public:
  LockDirectoryOp(MetaStore &meta, const LockDirectoryReq &req)
      : Operation<LockDirectoryRsp>(meta),
        req_(req) {}

  CoTryTask<LockDirectoryRsp> run(IReadWriteTransaction &txn) override {
    CO_RETURN_ON_ERROR(req_.valid());

    auto inode = (co_await Inode::load(txn, req_.inode)).then(checkMetaFound<Inode>);
    CO_RETURN_ON_ERROR(inode);
    CO_RETURN_ON_ERROR(inode->acl.checkPermission(req_.user, AccessType::WRITE));

    if (!inode->isDirectory()) {
      co_return makeError(MetaCode::kNotDirectory, fmt::format("{} is not directory", inode->id));
    }

    switch (req_.action) {
      case LockDirectoryReq::LockAction::TryLock:
        if (inode->asDirectory().lock && inode->asDirectory().lock->client.uuid != req_.client.uuid) {
          co_return makeError(MetaCode::kNoLock, fmt::format("lock hold by {}", *inode->asDirectory().lock));
        }
      case LockDirectoryReq::LockAction::PreemptLock:
        if (auto lock = Directory::Lock{req_.client}; inode->asDirectory().lock != lock) {
          inode->asDirectory().lock = lock;
          CO_RETURN_ON_ERROR(co_await inode->store(txn));
        }
        co_return LockDirectoryRsp();
      case LockDirectoryReq::LockAction::UnLock:
        if (!inode->asDirectory().lock) {
          co_return makeError(MetaCode::kNoLock, "locked not owned");
        }
        if (inode->asDirectory().lock->client.uuid != req_.client.uuid) {
          co_return makeError(MetaCode::kNoLock, fmt::format("lock hold by {}", *inode->asDirectory().lock));
        }
      case LockDirectoryReq::LockAction::Clear:
        if (inode->asDirectory().lock) {
          inode->asDirectory().lock = std::nullopt;
          CO_RETURN_ON_ERROR(co_await inode->store(txn));
        }
        co_return LockDirectoryRsp();
      default:
        XLOGF(DFATAL, "invalid action {}", (int)req_.action);
        co_return makeError(MetaCode::kFoundBug);
    }
  }

 private:
  const LockDirectoryReq &req_;
};

MetaStore::OpPtr<LockDirectoryRsp> MetaStore::lockDirectory(const LockDirectoryReq &req) {
  return std::make_unique<LockDirectoryOp>(*this, req);
}
}  // namespace hf3fs::meta::server