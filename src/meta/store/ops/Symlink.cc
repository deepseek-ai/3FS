#include <cassert>
#include <fcntl.h>
#include <folly/logging/xlog.h>
#include <memory>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "meta/event/Event.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"

namespace hf3fs::meta::server {

/** MetaStore::symlink */
class SymlinkOp : public Operation<SymlinkRsp> {
 public:
  SymlinkOp(MetaStore &meta, const SymlinkReq &req)
      : Operation<SymlinkRsp>(meta),
        req_(req) {}

  OPERATION_TAGS(req_);

  CoTryTask<SymlinkRsp> run(IReadWriteTransaction &txn) override {
    XLOGF(DBG, "SymlinkOp {}", req_);

    CHECK_REQUEST(req_);

    auto resolveResult = co_await resolve(txn, req_.user).path(req_.path, AtFlags(AT_SYMLINK_NOFOLLOW));
    CO_RETURN_ON_ERROR(resolveResult);
    if (resolveResult->dirEntry.has_value()) {
      auto &entry = *resolveResult->dirEntry;
      if (entry.uuid != Uuid::zero() && entry.uuid == req_.uuid) {
        // this may happens when FDB returns commit_unknown_result, or we failed to send response to client
        XLOGF(CRITICAL, "Symlink already created, dst {}, req {}, uuid {}", entry, req_, req_.uuid);
        auto inode = co_await entry.snapshotLoadInode(txn);
        CO_RETURN_ON_ERROR(inode);
        co_return SymlinkRsp(std::move(*inode));
      }
      co_return makeError(MetaCode::kExists);
    }

    // check permission and lock
    auto parent = co_await resolveResult->getParentInode(txn);
    CO_RETURN_ON_ERROR(parent);
    CO_RETURN_ON_ERROR(parent->acl.checkPermission(req_.user, AccessType::WRITE));
    CO_RETURN_ON_ERROR(parent->asDirectory().checkLock(req_.client));

    auto inodeId = co_await allocateInodeId(txn, false);
    CO_RETURN_ON_ERROR(inodeId);

    assert(req_.path.path.has_value());
    InodeId parentId = resolveResult->getParentId();
    DirEntry entry = DirEntry::newSymlink(parentId, req_.path.path->filename().native(), *inodeId);
    entry.uuid = req_.uuid;
    Inode inode = Inode::newSymlink(*inodeId, req_.target, req_.user.uid, req_.user.gid, now());

    // NOTE: add parent inode and dirEntry into read conflict set.
    // add parent inode into read conflict set to prevent parent is removed concurrently
    CO_RETURN_ON_ERROR(co_await Inode(parentId).addIntoReadConflict(txn));
    // add directory entry into read conflict set to prevent concurrent create
    CO_RETURN_ON_ERROR(co_await entry.addIntoReadConflict(txn));

    // create inode and dirEntry
    CO_RETURN_ON_ERROR(co_await entry.store(txn));
    CO_RETURN_ON_ERROR(co_await inode.store(txn));

    addEvent(Event::Type::Symlink)
        .addField("parent", entry.parent)
        .addField("name", entry.name)
        .addField("target", inode.asSymlink().target.native())
        .addField("user", req_.user.uid)
        .addField("host", req_.client.hostname);
    addTrace(MetaEventTrace{
        .eventType = Event::Type::Symlink,
        .parentId = entry.parent,
        .entryName = entry.name,
        .userId = req_.user.uid,
        .client = req_.client,
        .symLinkTarget = inode.asSymlink().target,
    });

    co_return SymlinkRsp(std::move(inode));
  }

 private:
  const SymlinkReq &req_;
};

MetaStore::OpPtr<SymlinkRsp> MetaStore::symlink(const SymlinkReq &req) {
  return std::make_unique<SymlinkOp>(*this, req);
}

}  // namespace hf3fs::meta::server
