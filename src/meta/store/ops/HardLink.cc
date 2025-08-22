#include <algorithm>
#include <cassert>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/logging/xlog.h>
#include <limits>
#include <linux/fs.h>
#include <memory>
#include <utility>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Service.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"
#include "meta/store/ops/SetAttr.h"

namespace hf3fs::meta::server {

class HardLinkOp : public Operation<HardLinkRsp> {
 public:
  HardLinkOp(MetaStore &meta, const HardLinkReq &req)
      : Operation<HardLinkRsp>(meta),
        req_(req) {}

  OPERATION_TAGS(req_);

  CoTryTask<HardLinkRsp> run(IReadWriteTransaction &txn) override {
    XLOGF(DBG, "HardLinkOp: {}", req_);

    CHECK_REQUEST(req_);
    // check name valid
    CO_RETURN_ON_ERROR(req_.newPath.validForCreate());

    auto resolveResult = co_await resolve(txn, req_.user).path(req_.newPath, req_.flags);
    CO_RETURN_ON_ERROR(resolveResult);
    if (resolveResult->dirEntry.has_value()) {
      auto &entry = *resolveResult->dirEntry;
      if (entry.uuid != Uuid::zero() && entry.uuid == req_.uuid) {
        // this may happens when FDB returns commit_unknown_result, or we failed to send response to client
        XLOGF(CRITICAL, "HardLink already created, dst {}, req {}, uuid {}", entry, req_, req_.uuid);
        auto inode = co_await entry.snapshotLoadInode(txn);
        CO_RETURN_ON_ERROR(inode);
        co_return HardLinkRsp(std::move(*inode));
      }
      co_return makeError(MetaCode::kExists, fmt::format("hardlink exists, req {}, uuid {}", req_, entry));
    }

    auto target = co_await resolve(txn, req_.user).inode(req_.oldPath, req_.flags, true /* checkRefCnt */);
    CO_RETURN_ON_ERROR(target);
    auto &inode = *target;

    // check permission and lock
    auto parent = co_await resolveResult->getParentInode(txn);
    CO_RETURN_ON_ERROR(parent);
    CO_RETURN_ON_ERROR(parent->acl.checkPermission(req_.user, AccessType::WRITE));
    CO_RETURN_ON_ERROR(parent->asDirectory().checkLock(req_.client));
    if (inode.acl.iflags & FS_IMMUTABLE_FL) {
      co_return makeError(MetaCode::kNoPermission, fmt::format("FS_IMMUTABLE_FL set on target inode {}", inode.id));
    }

    assert(inode.nlink);

    InodeId parentId = resolveResult->getParentId();
    DirEntry entry;
    switch (inode.getType()) {
      case InodeType::File:
        entry = DirEntry::newFile(parentId, req_.newPath.path->filename().native(), inode.id);
        break;
      case InodeType::Directory:
        co_return makeError(MetaCode::kIsDirectory);
      case InodeType::Symlink:
        entry = DirEntry::newSymlink(parentId, req_.newPath.path->filename().native(), inode.id);
        break;
      default:
        XLOGF(FATAL, "Found invalid inode type {}", (int)inode.getType());
    }

    entry.uuid = req_.uuid;

    if (inode.nlink == std::numeric_limits<uint16_t>::max()) {
      XLOGF(ERR, "Inode {} has {} links, can't add more hard link!", inode.id, inode.nlink);
      co_return makeError(MetaCode::kNoPermission, "nlink == uint16_t::max");
    }

    // NOTE: create dirEntry, add parent inode and dirEntry into read conflict set.
    // add parent inode into read conflict set to prevent parent is removed concurrently
    CO_RETURN_ON_ERROR(co_await Inode(parentId).addIntoReadConflict(txn));
    // add directory entry into read conflict set to prevent concurrent create
    CO_RETURN_ON_ERROR(co_await entry.addIntoReadConflict(txn));
    CO_RETURN_ON_ERROR(co_await entry.store(txn));

    // NOTE: add link count of inode, add inode into read conflict set since this is a read modify write
    assert(!inode.isDirectory());
    inode.nlink++;
    SetAttr::update(inode.ctime, UtcClock::now(), config().time_granularity(), true);
    CO_RETURN_ON_ERROR(co_await inode.addIntoReadConflict(txn));
    CO_RETURN_ON_ERROR(co_await inode.store(txn));

    addEvent(Event::Type::HardLink)
        .addField("parent", entry.parent)
        .addField("name", entry.name)
        .addField("inode", inode.id)
        .addField("owner", inode.acl.uid)
        .addField("user", req_.user.uid)
        .addField("host", req_.client.hostname)
        .addField("nlink", inode.nlink);
    addTrace(MetaEventTrace{
        .eventType = Event::Type::HardLink,
        .inodeId = inode.id,
        .parentId = entry.parent,
        .entryName = entry.name,
        .ownerId = inode.acl.uid,
        .userId = req_.user.uid,
        .client = req_.client,
        .nlink = inode.nlink,
    });
    co_return HardLinkRsp(std::move(inode));
  }

 private:
  const HardLinkReq &req_;
};

MetaStore::OpPtr<HardLinkRsp> MetaStore::hardLink(const HardLinkReq &req) {
  return std::make_unique<HardLinkOp>(*this, req);
}

}  // namespace hf3fs::meta::server
