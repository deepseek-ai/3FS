#include <algorithm>
#include <cassert>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/Likely.h>
#include <folly/ScopeGuard.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/fibers/BatchSemaphore.h>
#include <folly/fibers/Semaphore.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <iostream>
#include <limits>
#include <linux/fs.h>
#include <memory>
#include <optional>
#include <queue>
#include <utility>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/kv/WithTransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Service.h"
#include "fdb/FDBRetryStrategy.h"
#include "meta/components/GcManager.h"
#include "meta/components/SessionManager.h"
#include "meta/event/Event.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"
#include "meta/store/PathResolve.h"

namespace hf3fs::meta::server {

/** Remove. */
class RemoveOp : public Operation<RemoveRsp> {
 public:
  RemoveOp(MetaStore &meta, const RemoveReq &req)
      : Operation<RemoveRsp>(meta),
        req_(req) {}

  OPERATION_TAGS(req_);

  bool needIdempotent(Uuid &clientId, Uuid &requestId) const override {
    if (!req_.checkUuid()) {
      return false;
    }
    if (req_.recursive || config().idempotent_remove()) {
      clientId = req_.client.uuid;
      requestId = req_.uuid;
      return true;
    }
    return false;
  }

  CoTryTask<RemoveRsp> run(IReadWriteTransaction &txn) override {
    XLOGF(DBG, "RemoveOp: {}", req_);

    CHECK_REQUEST(req_);

    Result<PathResolveOp::ResolveResult> resolveResult = makeError(MetaCode::kFoundBug);
    if (req_.path.path.has_value()) {
      resolveResult = co_await resolve(txn, req_.user)
                          .path(req_.path, AtFlags(AT_SYMLINK_NOFOLLOW) /* remove shouldn't follow symlink */);
    } else {
      resolveResult = co_await resolve(txn, req_.user).byDirectoryInodeId(req_.path.parent);
    }
    CO_RETURN_ON_ERROR(resolveResult);

    if (!resolveResult->dirEntry.has_value()) {
      co_return makeError(MetaCode::kNotFound);
    } else if (resolveResult->dirEntry->id.isTreeRoot()) {
      // don't permit remove root
      co_return MAKE_ERROR_F(MetaCode::kNoPermission, "Can't remove tree root {}", *resolveResult->dirEntry);
    }
    // check src InodeId
    if (req_.inodeId && resolveResult->dirEntry->id != req_.inodeId) {
      co_return MAKE_ERROR_F(MetaCode::kNotFound, "remove {}, inodeId != {}", *resolveResult->dirEntry, *req_.inodeId);
    }

    // check permission, must have write permission to parent directory, and not locked
    auto parent = co_await resolveResult->getParentInode(txn);
    CO_RETURN_ON_ERROR(parent);
    CO_RETURN_ON_ERROR(parent->acl.checkPermission(req_.user, AccessType::WRITE));
    CO_RETURN_ON_ERROR(parent->asDirectory().checkLock(req_.client));
    auto &entry = resolveResult->dirEntry.value();
    if (req_.checkType) {
      if (req_.atFlags.contains(AT_REMOVEDIR) && entry.isFile()) {
        co_return makeError(MetaCode::kNotDirectory);
      }
      if (!req_.atFlags.contains(AT_REMOVEDIR) && entry.isDirectory()) {
        co_return makeError(MetaCode::kIsDirectory);
      }
    }

    // The sticky bit (S_ISVTX) on a directory means that a file in that directory can be renamed or deleted
    // only by the owner of the file, by the owner of the directory, and by a privileged process.
    auto loadInodeResult = co_await entry.snapshotLoadInode(txn);
    CO_RETURN_ON_ERROR(loadInodeResult);
    auto &inode = *loadInodeResult;
    if ((parent->acl.perm & S_ISVTX) && req_.user.uid != parent->acl.uid && !req_.user.isRoot() &&
        req_.user.uid != inode.acl.uid) {
      auto msg = fmt::format("can't remove {}, S_ISVTX set on parent {} {}", entry, parent->id, parent->acl);
      XLOG(DBG, msg);
      co_return makeError(MetaCode::kNoPermission, msg);
    }
    if (inode.acl.iflags & FS_IMMUTABLE_FL) {
      auto msg = fmt::format("can't remove {}, FS_IMMUTABLE_FL set on inode", entry);
      XLOG(DBG, msg);
      co_return makeError(MetaCode::kNoPermission, msg);
    }

    auto type = std::string(magic_enum::enum_name(inode.getType()));
    folly::toLowerAscii(type);

    auto event = Event(Event::Type::Remove);
    event.addField("parent", entry.parent)
        .addField("name", entry.name)
        .addField("inode", entry.id)
        .addField("type", type)
        .addField("owner", entry.dirAcl->uid)
        .addField("nlink", inode.nlink - 1)
        .addField("user", req_.user.uid)
        .addField("host", req_.client.hostname);
    auto trace = MetaEventTrace{.eventType = Event::Type::Remove,
                                .inodeId = entry.id,
                                .parentId = entry.parent,
                                .entryName = entry.name,
                                .ownerId = inode.acl.uid,
                                .userId = req_.user.uid,
                                .client = req_.client,
                                .inodeType = inode.getType(),
                                .nlink = inode.nlink,
                                .recursiveRemove = req_.recursive};

    auto gcInfo = GcInfo{req_.user.uid, entry.name};
    if (entry.isDirectory()) {
      event.addField("recursive", req_.recursive);

      auto result = co_await DirEntryList::checkEmpty(txn, entry.id);
      CO_RETURN_ON_ERROR(result);
      if (auto empty = *result; empty) {
        XLOGF_IF(DFATAL, inode.nlink != 1, "Directory {} nlink != 1", inode);
        // remove directory directly
        CO_RETURN_ON_ERROR(co_await entry.addIntoReadConflict(txn));
        CO_RETURN_ON_ERROR(co_await inode.addIntoReadConflict(txn));
        CO_RETURN_ON_ERROR(co_await entry.remove(txn));
        CO_RETURN_ON_ERROR(co_await inode.remove(txn));

        addEvent(event);
        addTrace(std::move(trace));

        co_return RemoveRsp{};
      }

      if (!req_.recursive) {
        co_return makeError(MetaCode::kNotEmpty);
      }

      CO_RETURN_ON_ERROR(inode.acl.checkRecursiveRmPerm(req_.user, config().recursive_remove_check_owner()));
      auto recursiveCheck = config().recursive_remove_perm_check();
      if (recursiveCheck) {
        auto res = co_await DirEntryList::recursiveCheckRmPerm(txn, inode.id, req_.user, recursiveCheck, 128);
        CO_RETURN_ON_ERROR(res);
      }

      // recursive remove, save original path
      auto ancestors = std::vector<Inode>();
      CO_RETURN_ON_ERROR(co_await Inode::loadAncestors(txn, ancestors, entry.parent));
      for (auto &ancestor : ancestors) {
        gcInfo.origPath = ancestor.asDirectory().name / gcInfo.origPath;
      }
      event.addField("origPath", gcInfo.origPath.string());
      trace.origPath = gcInfo.origPath;
    }

    auto result = co_await gcManager().removeEntry(txn, entry, inode, gcInfo);
    CO_RETURN_ON_ERROR(result);

    addEvent(event);
    addTrace(std::move(trace));

    co_return RemoveRsp{};
  }

 private:
  const RemoveReq &req_;
};

MetaStore::OpPtr<RemoveRsp> MetaStore::remove(const RemoveReq &req) { return std::make_unique<RemoveOp>(*this, req); }

}  // namespace hf3fs::meta::server
