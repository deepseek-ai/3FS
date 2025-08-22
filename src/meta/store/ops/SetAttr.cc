#include "SetAttr.h"

#include <fcntl.h>
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>
#include <optional>
#include <sys/stat.h>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Service.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"
#include "meta/store/PathResolve.h"

namespace hf3fs::meta::server {

class SetAttrOp : public Operation<SetAttrRsp> {
 public:
  SetAttrOp(MetaStore &meta, const SetAttrReq &req_)
      : Operation<SetAttrRsp>(meta),
        req_(req_) {}

  OPERATION_TAGS(req_);

  CoTryTask<SetAttrRsp> run(IReadWriteTransaction &txn) override {
    XLOGF(DBG, "SetAttrOp: {}", req_);

    Inode inode;
    std::optional<DirEntry> entry;
    if (req_.path.path.has_value()) {
      auto dirEntryResult = co_await resolve(txn, req_.user)
                                .dirEntry(req_.path, req_.flags | AT_SYMLINK_FOLLOW /* folly symlink by default*/);
      CO_RETURN_ON_ERROR(dirEntryResult);
      entry = std::move(*dirEntryResult);
      auto inodeResult = co_await entry->snapshotLoadInode(txn);
      CO_RETURN_ON_ERROR(inodeResult);
      inode = std::move(*inodeResult);
    } else {
      auto statResult =
          co_await resolve(txn, req_.user).inode(req_.path, req_.flags | AT_SYMLINK_FOLLOW, true /* checkRefCnt */);
      CO_RETURN_ON_ERROR(statResult);
      inode = std::move(*statResult);
    }

    auto dirty = false;
    auto oldAcl = inode.acl;

    CO_RETURN_ON_ERROR(SetAttr::check(inode, req_, config()));
    dirty |= SetAttr::apply(inode, req_, config().time_granularity(), config().dynamic_stripe_growth());

    if (inode.isDirectory() && inode.acl != oldAcl && inode.id != InodeId::root()) {
      XLOGF_IF(FATAL, !dirty, "acl changed but dirty not set, {} != {}", inode.acl, oldAcl);

      if (!entry.has_value() || entry->name == "." || entry->name == "..") {
        auto result = co_await inode.snapshotLoadDirEntry(txn);
        CO_RETURN_ON_ERROR(result);
        entry = std::move(*result);
        if (inode.asDirectory().name.empty()) {
          inode.asDirectory().name = entry->name;
          dirty = true;
        }
      }
      XLOGF_IF(DFATAL, entry->name != inode.asDirectory().name, "{} != {}", entry->name, inode.asDirectory().name);
      entry->dirAcl = inode.acl;
      CO_RETURN_ON_ERROR(co_await entry->addIntoReadConflict(txn));
      CO_RETURN_ON_ERROR(co_await entry->store(txn));
    }

    if (dirty) {
      // NOTE: add inode into read conflict set
      CO_RETURN_ON_ERROR(co_await inode.addIntoReadConflict(txn));
      CO_RETURN_ON_ERROR(co_await inode.store(txn));
    }

    co_return SetAttrRsp(std::move(inode));
  }

  void finish(const Result<SetAttrRsp> &result) override {
    Operation<SetAttrRsp>::finish(result);
    if (!result.hasError()) {
      if (req_.uid || req_.gid || req_.perm || req_.iflags) aclCache().invalid(result->stat.id);
    }
  }

 private:
  const SetAttrReq &req_;
};

MetaStore::OpPtr<SetAttrRsp> MetaStore::setAttr(const SetAttrReq &req_) {
  return std::make_unique<SetAttrOp>(*this, req_);
}

}  // namespace hf3fs::meta::server