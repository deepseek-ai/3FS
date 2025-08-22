#include <cassert>
#include <fcntl.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>

#include "common/kv/ITransaction.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "meta/components/GcManager.h"
#include "meta/components/SessionManager.h"
#include "meta/store/DirEntry.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"
#include "meta/store/PathResolve.h"
#include "meta/store/Utils.h"

#define BEGIN_WRITE()                                                                              \
  if (this->isReadOnly()) {                                                                        \
    auto msg = fmt::format("Op {}{} shouldn't be readonly!", MetaSerde<>::getRpcName(req_), req_); \
    XLOG(DFATAL, msg);                                                                             \
    co_return makeError(MetaCode::kFoundBug, std::move(msg));                                      \
  }                                                                                                \
  auto &rwTxn = dynamic_cast<IReadWriteTransaction &>(txn);

namespace hf3fs::meta::server {
monitor::CountRecorder openWrite("meta_server.open_write");

/** MetaStore::open */
template <typename Req, typename Rsp>
class OpenOp : public Operation<Rsp> {
 public:
  OpenOp(MetaStore &meta, Req &req)
      : Operation<Rsp>(meta),
        req_(req) {}

  OPERATION_TAGS(req_);

  bool isReadOnly() final {
    return !req_.session.has_value() && req_.flags.accessType() == AccessType::READ && !req_.flags.contains(O_TRUNC) &&
           !req_.flags.contains(O_CREAT);
  }

  CoTryTask<Rsp> run(IReadWriteTransaction &txn) final {
    XLOGF(DBG, "OpenOp: {}", req_);

    CHECK_REQUEST(req_);

    if (!req_.path.path.has_value()) {
      // open by inodeId
      auto inode =
          co_await this->resolve(txn, req_.user)
              .inode(req_.path, AtFlags(AT_SYMLINK_FOLLOW) /* open/create follow symlink*/, true /* checkRefCnt */);
      CO_RETURN_ON_ERROR(inode);
      co_return co_await openExists(txn, std::nullopt, std::move(*inode), this->config().check_file_hole());
    } else {
      // open by path, can handle O_TRUNC by replace inode here.
      auto resolveResult = co_await this->resolve(txn, req_.user).path(req_.path, AtFlags(AT_SYMLINK_FOLLOW));
      CO_RETURN_ON_ERROR(resolveResult);
      auto &entry = resolveResult->dirEntry;
      if (!entry.has_value()) {
        if constexpr (std::is_same_v<Req, CreateReq>) {
          req_.path = PathAt(resolveResult->getParentId(), req_.path.path->filename());
        }
        co_return makeError(MetaCode::kNotFound);
      }
      assert(!entry->isSymlink());
      auto inode = co_await entry->snapshotLoadInode(txn);
      CO_RETURN_ON_ERROR(inode);
      co_return co_await openExists(txn, std::move(*entry), std::move(*inode), this->config().check_file_hole());
    }
  }

  CoTryTask<Rsp> openExists(IReadOnlyTransaction &txn, std::optional<DirEntry> entry, Inode inode, bool checkHole) {
    XLOGF(DBG, "inode {}", inode);
    assert(!entry.has_value() || inode.id == entry->id);
    if (prevCreatedInodeId_ == inode.id) {
      // this inode is created by us, just return here.
      if (entry.has_value()) {
        addCreateEvent(*entry, inode);
      }
      co_return Rsp(std::move(inode), false);
    }

    if (req_.flags.contains(O_EXCL)) {
      co_return makeError(MetaCode::kExists);
    }

    switch (inode.getType()) {
      case InodeType::Directory:
        co_return co_await openExistsDirectory(txn, inode);
      case InodeType::File:
        co_return co_await openExistsFile(txn, entry, inode, checkHole);
      default:
        XLOGF(FATAL, "inode {} invalid type {}", inode, (int)inode.getType());
    }
  }

  CoTryTask<Rsp> openExistsDirectory(IReadOnlyTransaction &txn, Inode &inode) {
    XLOGF_IF(FATAL, !inode.isDirectory(), "Inode {} is not directory", inode);
    if (req_.flags.accessType() != AccessType::READ || req_.flags.contains(O_TRUNC) || std::is_same_v<Req, CreateReq>) {
      co_return makeError(MetaCode::kIsDirectory);
    }
    CO_RETURN_ON_ERROR(inode.acl.checkPermission(req_.user, req_.flags.accessType()));
    co_return Rsp(std::move(inode), false);
  }

  CoTryTask<Rsp> openExistsFile(IReadOnlyTransaction &txn,
                                std::optional<DirEntry> &entry,
                                Inode &inode,
                                bool checkHole) {
    XLOGF_IF(FATAL, !inode.isFile(), "Inode {} is not file", inode);

    bool dirty = false;

    // check permission
    if (req_.flags.contains(O_DIRECTORY)) {
      co_return makeError(MetaCode::kNotDirectory);
    }
    if (req_.flags.accessType() != AccessType::READ && (inode.acl.iflags & FS_IMMUTABLE_FL)) {
      co_return makeError(MetaCode::kNoPermission, fmt::format("FS_IMMUTABLE_FL set on inode {}", inode.id));
    }
    CO_RETURN_ON_ERROR(inode.acl.checkPermission(req_.user, req_.flags.accessType()));

    // check hole
    auto rdonly = req_.flags.accessType() == AccessType::READ;
    if (rdonly && inode.asFile().hasHole() && checkHole) {
      XLOGF(WARN, "Inode {} contains hole, don't allow O_RDONLY", inode.id);
      co_return makeError(MetaCode::kFileHasHole);
    }

    // handle otrunc
    bool otrunc = req_.flags.contains(O_TRUNC);
    XLOGF(DBG, "inode {}, otrunc {}", inode, otrunc);
    if (otrunc && entry.has_value()) {
      BEGIN_WRITE();
      auto replaced = co_await replaceExistsFile(rwTxn, *entry, inode);
      CO_RETURN_ON_ERROR(replaced);
      if (*replaced) {
        co_return Rsp(std::move(inode), false);
      }
    }

    // clear SUID SGID sticky bits on write by non owner
    constexpr uint32_t sbits = S_ISUID | S_ISGID | S_ISVTX;
    static_assert(sbits == 07000);
    if (!rdonly && req_.user.uid != inode.acl.uid && (inode.acl.perm & sbits)) {
      inode.acl.perm = Permission(inode.acl.perm & (~sbits));
      dirty = true;
    }

    if (req_.session.has_value() && req_.flags.accessType() != AccessType::READ) {
      BEGIN_WRITE();
      CO_RETURN_ON_ERROR(co_await createSession(rwTxn, inode, req_.flags));

      if (!req_.dynStripe && inode.asFile().dynStripe && inode.asFile().dynStripe < inode.asFile().layout.stripeSize) {
        inode.asFile().dynStripe = 0;
        dirty = true;
      }
    }

    if (dirty) {
      BEGIN_WRITE();
      CO_RETURN_ON_ERROR(co_await inode.addIntoReadConflict(rwTxn));
      CO_RETURN_ON_ERROR(co_await inode.store(rwTxn));
    }

    XLOGF(DBG, "inode {}, otrunc {}", inode, otrunc);

    co_return Rsp(std::move(inode), otrunc);
  }

  CoTryTask<bool> replaceExistsFile(kv::IReadWriteTransaction &txn, DirEntry &entry, Inode &inode) {
    XLOGF(DBG, "Try to replace file {} on O_TRUNC", entry);
    assert(inode.isFile());
    if (!this->config().otrunc_replace_file() || inode.nlink != 1 ||
        inode.asFile().length < this->config().otrunc_replace_file_threshold()) {
      XLOGF(DBG,
            "Can't replace {}, enable replace {}, nlink {}, size {}",
            entry,
            this->config().otrunc_replace_file(),
            inode.nlink,
            inode.asFile().length);
      co_return false;
    }
    auto checkResult = co_await FileSession::checkExists(txn, inode.id);
    CO_RETURN_ON_ERROR(checkResult);
    if (*checkResult) {
      XLOGF(DBG, "Can't replace {}, has session", entry);
      co_return false;
    }

    XLOGF(DBG, "Replace {} with a new inode", entry);
    auto old = inode;
    CO_RETURN_ON_ERROR(co_await this->gcManager().removeEntry(txn, entry, old, GcInfo{req_.user.uid, entry.name}));

    // create new entry and inode
    auto inodeId = co_await this->allocateInodeId(txn, false);
    CO_RETURN_ON_ERROR(inodeId);
    entry = DirEntry::newFile(entry.parent, std::string(entry.name), *inodeId);
    inode = Inode::newFile(*inodeId, inode.acl, inode.asFile().layout, this->now());
    if (this->config().dynamic_stripe() && req_.dynStripe) {
      inode.asFile().dynStripe = std::min(this->config().dynamic_stripe_initial(), inode.asFile().layout.stripeSize);
    }

    CO_RETURN_ON_ERROR(co_await createInodeAndEntry(txn, entry, inode, old));
    CO_RETURN_ON_ERROR(co_await createSession(txn, inode, req_.flags));

    co_return true;
  }

  CoTryTask<Void> createInodeAndEntry(IReadWriteTransaction &txn,
                                      DirEntry &entry,
                                      Inode &inode,
                                      std::optional<Inode> old = std::nullopt) {
    auto parentId = entry.parent;
    auto inodeId = entry.id;
    assert(inode.id == inodeId);

    // NOTE: add parent inode and dirEntry into read conflict set.
    // add parent inode into read conflict set to prevent parent is removed concurrently
    CO_RETURN_ON_ERROR(co_await Inode(parentId).addIntoReadConflict(txn));
    // add directory entry into read conflict set to prevent concurrent create
    CO_RETURN_ON_ERROR(co_await entry.addIntoReadConflict(txn));

    // create inode and dirEntry
    CO_RETURN_ON_ERROR(co_await entry.store(txn));
    CO_RETURN_ON_ERROR(co_await inode.store(txn));
    prevCreatedInodeId_ = inodeId;

    addCreateEvent(entry, inode, old);
    co_return Void{};
  }

  CoTryTask<Void> createSession(IReadWriteTransaction &txn, Inode &inode, OpenFlags oflags) {
    if (!inode.isFile()) {
      assert(false);
      co_return makeError(MetaCode::kNotFile);
    }
    if (!req_.session.has_value()) {
      co_return Void{};
    }

    openWrite.addSample(1);
    auto session = FileSession::create(inode.id, req_.session.value());
    CO_RETURN_ON_ERROR(co_await session.store(txn));

    this->addEvent(Event::Type::OpenWrite)
        .addField("inode", inode.id)
        .addField("owner", inode.acl.uid)
        .addField("user", req_.user.uid)
        .addField("host", req_.client.hostname)
        .addField("length", inode.asFile().length)
        .addField("truncateVer", inode.asFile().truncateVer)
        .addField("dynStripe", inode.asFile().dynStripe)
        .addField("otrunc", oflags.contains(O_TRUNC));
    this->addTrace(MetaEventTrace{
        .eventType = Event::Type::OpenWrite,
        .inodeId = inode.id,
        .ownerId = inode.acl.uid,
        .userId = req_.user.uid,
        .client = req_.client,
        .length = inode.asFile().length,
        .truncateVer = inode.asFile().truncateVer,
        .dynStripe = inode.asFile().dynStripe,
        .oflags = oflags,
    });

    co_return Void{};
  }

  void addCreateEvent(const DirEntry &entry, const Inode &inode, std::optional<Inode> old = std::nullopt) {
    XLOGF_IF(DFATAL, (old.has_value() && !old->isFile()), "old {} is not file", *old);
    auto &event = this->addEvent(Event::Type::Create)
                      .addField("parent", entry.parent)
                      .addField("name", entry.name)
                      .addField("inode", entry.id)
                      .addField("user", req_.user.uid)
                      .addField("host", req_.client.hostname)
                      .addField("chain_table", inode.asFile().layout.tableId);
    if (old && old->isFile()) {
      event.addField("old_inode", old->id).addField("old_length", old->asFile().length);
    }
    this->addTrace(MetaEventTrace{
        .eventType = Event::Type::Create,
        .inodeId = entry.id,
        .parentId = entry.parent,
        .entryName = entry.name,
        .userId = req_.user.uid,
        .client = req_.client,
        .tableId = inode.asFile().layout.tableId,
    });
  }

 private:
  Req &req_;
  std::optional<InodeId> prevCreatedInodeId_;
};

MetaStore::OpPtr<OpenRsp> MetaStore::open(OpenReq &req) {
  return std::make_unique<OpenOp<OpenReq, OpenRsp>>(*this, req);
}

MetaStore::OpPtr<CreateRsp> MetaStore::tryOpen(CreateReq &req) {
  return std::make_unique<OpenOp<CreateReq, CreateRsp>>(*this, req);
}

}  // namespace hf3fs::meta::server
