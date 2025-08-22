#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>
#include <iterator>
#include <memory>
#include <optional>
#include <sys/stat.h>

#include "common/utils/FaultInjection.h"
#include "common/utils/Result.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Common.h"
#include "meta/store/DirEntry.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"

namespace hf3fs::meta::server {

/** MetaStore::mkdirs */
class MkdirsOp : public Operation<MkdirsRsp> {
 public:
  MkdirsOp(MetaStore &meta, const MkdirsReq &req)
      : Operation<MkdirsRsp>(meta),
        req_(req) {}

  OPERATION_TAGS(req_);

  CoTryTask<MkdirsRsp> run(IReadWriteTransaction &txn) override {
    XLOGF(DBG, "MkdirsOp: {}", req_);

    CHECK_REQUEST(req_);

    auto resolveResult = co_await resolve(txn, req_.user).pathRange(req_.path);
    CO_RETURN_ON_ERROR(resolveResult);
    auto curr = resolveResult->missing.begin();
    const auto end = resolveResult->missing.end();

    if (curr == end) {
      assert(resolveResult->dirEntry.has_value());
      if (resolveResult->dirEntry->uuid != Uuid::zero() && resolveResult->dirEntry->uuid == req_.uuid) {
        // this may happens when FDB returns commit_unknown_result, or we failed to send response to client
        XLOGF(CRITICAL, "Mkdirs already finished, dst {}, req {}, uuid {}", *resolveResult->dirEntry, req_, req_.uuid);
        auto inode = co_await resolveResult->dirEntry->snapshotLoadInode(txn);
        CO_RETURN_ON_ERROR(inode);
        co_return MkdirsRsp(std::move(*inode));
      }
      co_return makeError(MetaCode::kExists);
    }

    if (std::distance(curr, end) != 1 && !req_.recursive) {
      // some middle path components are missing and not recursive mkdirs
      co_return makeError(MetaCode::kNotFound);
    }

    auto parent = co_await resolveResult->getParentInode(txn);
    CO_RETURN_ON_ERROR(parent);
    CO_RETURN_ON_ERROR(parent->acl.checkPermission(req_.user, AccessType::WRITE));
    CO_RETURN_ON_ERROR(parent->asDirectory().checkLock(req_.client));

    auto layout = req_.layout;
    if (!layout.has_value()) {
      // user doesn't specific layout, inherit parent directory's layout.
      layout = parent->asDirectory().layout;
    }

    InodeId parentId = resolveResult->getParentId();
    // NOTE: add parent inode and dirEntry into read conflict set.
    // add parent inode into read conflict set to prevent parent is removed concurrently
    CO_RETURN_ON_ERROR(co_await Inode(parentId).addIntoReadConflict(txn));
    // add directory entry into read conflict set to prevent concurrent create
    CO_RETURN_ON_ERROR(co_await DirEntry(parentId, curr->native()).addIntoReadConflict(txn));

    auto acl = Acl(req_.user.uid,
                   req_.user.gid,
                   Permission(req_.perm & ALLPERMS),
                   IFlags(parent->acl.iflags & FS_FL_INHERITABLE));
    if (parent->acl.perm & S_ISGID) {
      // The set-group-ID bit (S_ISGID) has several special uses.
      // For a directory, it indicates that BSD semantics are to be used for that directory:
      // files created there inherit their group ID from the  directory, not from the effective group ID of the creating
      // process, and directories created there will also get the S_ISGID bit set
      acl.gid = parent->acl.gid;
      acl.perm = Permission(acl.perm | S_ISGID);
    }

    // create all path components
    FAULT_INJECTION_SET_FACTOR(std::distance(curr, end));
    while (true) {
      if (!curr->has_filename() || curr->filename_is_dot() || curr->filename_is_dot_dot()) {
        co_return makeError(StatusCode::kInvalidArg, "filename is '.' or '..'");
      }

      auto inodeId = co_await allocateInodeId(txn, false);
      CO_RETURN_ON_ERROR(inodeId);

      CO_RETURN_ON_ERROR(co_await chainAlloc().checkLayoutValid(layout.value()));

      Inode inode = Inode::newDirectory(*inodeId, parentId, curr->filename().native(), acl, *layout, now());
      DirEntry entry = DirEntry::newDirectory(parentId, curr->native(), *inodeId, acl);
      entry.uuid = req_.uuid;

      // create inode and dirEntry
      CO_RETURN_ON_ERROR(co_await entry.store(txn));
      CO_RETURN_ON_ERROR(co_await inode.store(txn));

      addEvent(Event::Type::Mkdir)
          .addField("parent", entry.parent)
          .addField("name", entry.name)
          .addField("inode", inode.id)
          .addField("user", inode.acl.uid)
          .addField("host", req_.client.hostname)
          .addField("chain_table", inode.asDirectory().layout.tableId);
      addTrace(MetaEventTrace{
          .eventType = Event::Type::Mkdir,
          .inodeId = inode.id,
          .parentId = entry.parent,
          .entryName = entry.name,
          .userId = inode.acl.uid,
          .client = req_.client,
          .tableId = inode.asDirectory().layout.tableId,
      });

      curr++;
      parentId = *inodeId;
      if (curr == end) {
        co_return MkdirsRsp(std::move(inode));
      }
    }
  }

 private:
  const MkdirsReq &req_;
};

MetaStore::OpPtr<MkdirsRsp> MetaStore::mkdirs(const MkdirsReq &req) { return std::make_unique<MkdirsOp>(*this, req); }

}  // namespace hf3fs::meta::server
