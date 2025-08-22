#include <algorithm>
#include <cassert>
#include <fcntl.h>
#include <folly/Likely.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/logging/xlog.h>
#include <linux/fs.h>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Service.h"
#include "meta/components/GcManager.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"
#include "meta/store/PathResolve.h"
#include "meta/store/Utils.h"
#include "meta/store/ops/SetAttr.h"

namespace hf3fs::meta::server {

/** MetaStore::rename */
/**
 * Note: rename operation in POSIX and HDFS has different semantic when destination exists.
 * In POSIX, if destination is a file or empty directory, it will be replaced automatilally (special case:
 * set RENAME_NOREPLACE flags in renameat2).
 * In HDFS,
 * - if destination is a file, rename operation will raise FileAlreadyExistsException;
 * - if destination is a directory and source is file, source will be moved under destination (eg: mv file dir ->
 * dir/file);
 * - if both source and destination are directories, all children of source will be moved under destination recursively
 * (we have decided to not provide this semantic because it's too complicated).
 *
 * This function implements POSIX semantic.
 */
class RenameOp : public Operation<RenameRsp> {
 public:
  RenameOp(MetaStore &meta, const RenameReq &req)
      : Operation<RenameRsp>(meta),
        req_(req) {}

  OPERATION_TAGS(req_);

  static bool underTrash(const std::vector<Inode> &ancestors) {
    return ancestors.size() >= 2 && ancestors[ancestors.size() - 1].id == InodeId::root() &&
           ancestors[ancestors.size() - 2].asDirectory().name == "trash";
  }

  bool needIdempotent(Uuid &clientId, Uuid &requestId) const override {
    if (!req_.checkUuid()) return false;
    if (!req_.moveToTrash && !config().idempotent_rename()) return false;
    clientId = req_.client.uuid;
    requestId = req_.uuid;
    return true;
  }

  CoTryTask<Void> checkLoop(IReadWriteTransaction &txn,
                            const PathResolveOp::ResolveResult &srcResult,
                            const PathResolveOp::ResolveResult &dstResult,
                            std::optional<Path> &origPath) {
    auto dstAncestors = std::vector<Inode>();
    CO_RETURN_ON_ERROR(co_await Inode::loadAncestors(txn, dstAncestors, dstResult.getParentId()));
    assert(!dstAncestors.empty());

    for (auto &ancestor : dstAncestors) {
      // src is not dst's ancestor
      if (ancestor.id == srcResult.dirEntry->id) {
        // try to move directory into it's descendent
        co_return makeError(StatusCode::kInvalidArg, "try to move directory into it's descendent");
      }

      // move into a deleted directory
      if (ancestor.nlink == 0) {
        co_return makeError(MetaCode::kNotFound);
      }

      // check root
      if (ancestor.id == ancestor.asDirectory().parent) {
        if (ancestor.id == InodeId::root()) {
          break;
        } else if (ancestor.id == InodeId::gcRoot()) {
          XLOGF(ERR, "RenameOp: {} move directory into a removed directory", req_);
          co_return makeError(MetaCode::kNoPermission);
        } else {
          XLOGF(DFATAL, "Inode {} parent is itself", ancestor);
          co_return makeError(MetaCode::kFoundBug);
        }
      }
    }

    if (underTrash(dstAncestors)) {
      XLOGF_IF(FATAL, !srcResult.dirEntry->isDirectory(), "{} not directory", *srcResult.dirEntry);
      auto srcAncestors = std::vector<Inode>();
      CO_RETURN_ON_ERROR(co_await Inode::loadAncestors(txn, srcAncestors, srcResult.getParentId()));

      if (req_.moveToTrash || config().allow_directly_move_to_trash()) {
        auto acl = srcResult.dirEntry->dirAcl;
        if (!acl) {
          XLOGF(DFATAL, "DirEntry {} is directory, but don't have acl", *srcResult.dirEntry);
          co_return makeError(MetaCode::kFoundBug);
        }
        // try to move a directory into trash directory, should be owner and have rwx permission
        CO_RETURN_ON_ERROR(acl->checkRecursiveRmPerm(req_.user, config().recursive_remove_check_owner()));

        auto recursiveCheck = config().recursive_remove_perm_check();
        if (recursiveCheck) {
          auto res =
              co_await DirEntryList::recursiveCheckRmPerm(txn, srcResult.dirEntry->id, req_.user, recursiveCheck, 128);
          CO_RETURN_ON_ERROR(res);
        }
      } else if (req_.user.uid != flat::Uid(0)) {
        // src should already in trash
        if (!underTrash(srcAncestors)) {
          co_return makeError(MetaCode::kNoPermission, "try to move into trash directory without moveToTrash");
        }
      }

      origPath = Path(srcResult.dirEntry->name);
      for (auto &ancestor : srcAncestors) {
        origPath = ancestor.asDirectory().name / *origPath;
      }
    }

    co_return Void{};
  }

  CoTryTask<Void> snapshotLoadInode(IReadWriteTransaction &txn, const DirEntry &entry, std::optional<Inode> &inode) {
    if (!inode.has_value()) {
      auto result = co_await entry.snapshotLoadInode(txn);
      CO_RETURN_ON_ERROR(result);
      inode = std::move(*result);
    }
    co_return Void{};
  }

  CoTryTask<Void> checkPermission(IReadWriteTransaction &txn,
                                  PathResolveOp::ResolveResult &resolve,
                                  std::optional<Inode> &inode,
                                  bool dst) {
    auto parent = co_await resolve.getParentInode(txn);
    CO_RETURN_ON_ERROR(parent);
    CO_RETURN_ON_ERROR(parent->acl.checkPermission(req_.user, AccessType::WRITE));
    CO_RETURN_ON_ERROR(parent->asDirectory().checkLock(req_.client));
    if (dst && !parent->nlink) {
      // can't rename into a removed directory
      co_return makeError(MetaCode::kNotFound);
    }
    if (!resolve.dirEntry.has_value()) {
      co_return Void{};
    }

    auto &entry = *resolve.dirEntry;
    CO_RETURN_ON_ERROR(co_await snapshotLoadInode(txn, entry, inode));
    if (inode->acl.iflags & FS_IMMUTABLE_FL) {
      auto msg = fmt::format("rename can't move {}, FS_IMMUTABLE_FL set on inode", entry);
      XLOG(DBG, msg);
      co_return makeError(MetaCode::kNoPermission, msg);
    }

    // The sticky bit (S_ISVTX) on a directory means that a file in that directory can be renamed or deleted
    // only by the owner of the file, by the owner of the directory, and by a privileged process.
    if ((parent->acl.perm & S_ISVTX) && req_.user.uid != parent->acl.uid && !req_.user.isRoot()) {
      // not owner of directory and not owner of privileged process, should be owner of file
      if (req_.user.uid != inode->acl.uid) {
        auto msg = fmt::format("rename can't move {} {}, S_ISVTX set on parent {} {}",
                               entry,
                               inode->acl,
                               resolve.getParentId(),
                               parent->acl);
        XLOG(DBG, msg);
        co_return makeError(MetaCode::kNoPermission, msg);
      }
    }

    co_return Void{};
  }

  CoTryTask<std::optional<std::pair<InodeId, uint16_t>>> removeDst(IReadWriteTransaction &txn,
                                                                   PathResolveOp::ResolveResult &dst,
                                                                   std::optional<Inode> &dstInode) {
    if (!dst.dirEntry.has_value()) {
      co_return std::nullopt;
    }

    assert(dst.dirEntry->name == req_.dest.path->filename().native());
    if (dst.dirEntry->isFile()) {
      // let GC task free file chunks.
      CO_RETURN_ON_ERROR(co_await snapshotLoadInode(txn, *dst.dirEntry, dstInode));
      CO_RETURN_ON_ERROR(
          co_await gcManager().removeEntry(txn, *dst.dirEntry, *dstInode, GcInfo{req_.user.uid, dst.dirEntry->name}));
      assert(dstInode->id == dst.dirEntry->id);
      co_return std::pair<InodeId, uint16_t>{dstInode->id, dstInode->nlink};
    } else if (dst.dirEntry->isDirectory()) {
      // empty directory, can remove Inode directly
      CO_RETURN_ON_ERROR(co_await Inode(dst.dirEntry->id).remove(txn));
      co_return std::pair<InodeId, uint16_t>{dst.dirEntry->id, 0};
    } else {
      XLOGF_IF(DFATAL, !dst.dirEntry->isSymlink(), "{} not symlink, shouldn't happen", *dst.dirEntry);
      // need load inode and check refcnt
      auto inode = co_await dst.dirEntry->loadInode(txn);
      CO_RETURN_ON_ERROR(inode);
      if (UNLIKELY(inode->nlink == 0)) {
        auto msg = fmt::format("entry {} exists, but inode {} nlink == 0", *dst.dirEntry, inode);
        XLOG(DFATAL, msg);
        co_return makeError(MetaCode::kFoundBug, msg);
      }
      // NOTE: The fuse client may have cached this symlink. If delete it immediately, kNotFound will be reported for
      // subsequent visits. The temporary solution is not to delete the symlink inode. This problem needs to be resolved
      // later.
      SetAttr::update(inode->ctime, UtcClock::now(), config().time_granularity(), true);
      auto refcnt = --inode->nlink;
      CO_RETURN_ON_ERROR(co_await inode->store(txn));
      // if (refcnt != 0) {
      //   CO_RETURN_ON_ERROR(co_await inode->store(txn));
      // } else {
      //   CO_RETURN_ON_ERROR(co_await inode->remove(txn));
      // }
      co_return std::pair<InodeId, uint16_t>{dst.dirEntry->id, refcnt};
    }
  }

  CoTryTask<RenameRsp> run(IReadWriteTransaction &txn) override {
    XLOGF(DBG, "RenameOp: {}", req_);

    CHECK_REQUEST(req_);

    auto [srcResult, dstResult] =
        co_await folly::coro::collectAll(resolve(txn, req_.user).path(req_.src, AtFlags(AT_SYMLINK_NOFOLLOW)),
                                         resolve(txn, req_.user).path(req_.dest, AtFlags(AT_SYMLINK_NOFOLLOW)));
    CO_RETURN_ON_ERROR(srcResult);
    CO_RETURN_ON_ERROR(dstResult);

    // check dst, transaction may already executed.
    if (dstResult->dirEntry.has_value() && dstResult->dirEntry->uuid != Uuid::zero() &&
        dstResult->dirEntry->uuid == req_.uuid) {
      // this may happens when FDB returns commit_unknown_result, or we failed to send response to client
      XLOGF(CRITICAL, "Rename already finished, dst {}, req {}, uuid {}", *dstResult->dirEntry, req_, req_.uuid);
      auto inode = co_await dstResult->dirEntry->snapshotLoadInode(txn);
      CO_RETURN_ON_ERROR(inode);
      co_return RenameRsp(std::move(*inode));
    }

    // src should exists
    if (!srcResult->dirEntry.has_value()) {
      co_return MAKE_ERROR_F(MetaCode::kNotFound, "rename src {} not found", req_.src);
    }
    // check src InodeId
    if (req_.inodeId && srcResult->dirEntry->id != req_.inodeId) {
      co_return MAKE_ERROR_F(MetaCode::kNotFound, "rename src {}, inodeId != {}", *srcResult->dirEntry, *req_.inodeId);
    }
    // if src and dst points to same dir entry, do nothing
    if (dstResult->dirEntry.has_value() && dstResult->dirEntry->parent == srcResult->dirEntry->parent &&
        dstResult->dirEntry->name == srcResult->dirEntry->name) {
      auto inode = co_await dstResult->dirEntry->snapshotLoadInode(txn);
      CO_RETURN_ON_ERROR(inode);
      co_return RenameRsp(std::move(*inode));
    }
    // move to trash shouldn't replace file already exists
    if (dstResult->dirEntry.has_value() && dstResult->dirEntry->isFile() && req_.moveToTrash) {
      co_return MAKE_ERROR_F(MetaCode::kExists, "rename dest {} exist", req_.dest);
    }
    // dst shouldn't be a non-empty directory
    if (dstResult->dirEntry.has_value() && dstResult->dirEntry->isDirectory()) {
      auto checkResult = co_await DirEntryList::checkEmpty(txn, dstResult->dirEntry->id);
      CO_RETURN_ON_ERROR(checkResult);
      bool empty = checkResult.value();
      if (!empty) {
        co_return MAKE_ERROR_F(MetaCode::kNotEmpty, "rename dest {} not empty", req_.dest);
      }
    }
    // now, dst can be safely replaced (not exist, empty directory, file, symlink).
    std::optional<Path> origPath;
    if (srcResult->dirEntry->isDirectory()) {
      if (dstResult->dirEntry.has_value() && !dstResult->dirEntry->isDirectory()) {
        // man 2 rename: oldpath can specify a directory. In this case, newpath must either not exist, or it must
        // specify an empty directory.
        co_return makeError(MetaCode::kNotDirectory);
      }
      CO_RETURN_ON_ERROR(co_await checkLoop(txn, *srcResult, *dstResult, origPath));
    }

    // permission check
    std::optional<Inode> srcInode, dstInode;
    CO_RETURN_ON_ERROR(co_await checkPermission(txn, *srcResult, srcInode, false));
    CO_RETURN_ON_ERROR(co_await checkPermission(txn, *dstResult, dstInode, true));

    // NOTE: add src/dst's parent inode and dirEntry into read conflict set.
    CO_RETURN_ON_ERROR(co_await Inode(srcResult->getParentId()).addIntoReadConflict(txn));
    CO_RETURN_ON_ERROR(co_await srcResult->dirEntry->addIntoReadConflict(txn));
    CO_RETURN_ON_ERROR(co_await Inode(dstResult->getParentId()).addIntoReadConflict(txn));
    CO_RETURN_ON_ERROR(
        co_await DirEntry(dstResult->getParentId(), req_.dest.path->filename().native()).addIntoReadConflict(txn));

    auto &srcEntry = srcResult->dirEntry.value();
    auto inodeResult = co_await srcEntry.loadInode(txn);
    CO_RETURN_ON_ERROR(inodeResult);
    auto &inode = inodeResult.value();
    if (srcEntry.isDirectory()) {
      // NOTE: add src's inode into read conflict set.
      // load inode and update it's parent, read modify write, should use load.
      inode.asDirectory().parent = dstResult->getParentId();
      inode.asDirectory().name = req_.dest.path->filename().native();
      auto updateInodeResult = co_await inode.store(txn);
      CO_RETURN_ON_ERROR(updateInodeResult);
    }

    // remove src entry and dst entry
    CO_RETURN_ON_ERROR(co_await srcEntry.remove(txn));
    auto removeDstResult = co_await removeDst(txn, *dstResult, dstInode);
    CO_RETURN_ON_ERROR(removeDstResult);
    auto &oldDst = *removeDstResult;

    // create dst entry
    DirEntry newDstEntry(dstResult->getParentId(), req_.dest.path->filename().native());
    newDstEntry.data() = srcEntry.data();
    newDstEntry.uuid = req_.uuid;
    CO_RETURN_ON_ERROR(co_await newDstEntry.store(txn));

    auto &event = addEvent(Event::Type::Rename)
                      .addField("srcParent", srcEntry.parent)
                      .addField("srcName", srcEntry.name)
                      .addField("dstParent", newDstEntry.parent)
                      .addField("dstName", newDstEntry.name)
                      .addField("inode", newDstEntry.id)
                      .addField("user", req_.user.uid)
                      .addField("host", req_.client.hostname);
    addTrace(MetaEventTrace{.eventType = Event::Type::Rename,
                            .inodeId = newDstEntry.id,
                            .parentId = srcEntry.parent,
                            .entryName = srcEntry.name,
                            .dstParentId = newDstEntry.parent,
                            .dstEntryName = newDstEntry.name,
                            .userId = req_.user.uid,
                            .client = req_.client,
                            .origPath = origPath.value_or(Path())});

    if (oldDst.has_value()) {
      auto [oldDstInode, oldDstNlink] = *oldDst;
      event.addField("oldDstInode", oldDstInode).addField("oldDstNlink", oldDstNlink);
    }

    if (origPath.has_value()) {
      event.addField("origPath", origPath->string());
    }

    co_return RenameRsp(std::move(inode));
  }

 private:
  const RenameReq &req_;
};

MetaStore::OpPtr<RenameRsp> MetaStore::rename(const RenameReq &req) { return std::make_unique<RenameOp>(*this, req); }

}  // namespace hf3fs::meta::server
