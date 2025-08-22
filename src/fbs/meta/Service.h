#pragma once

#include <fcntl.h>
#include <folly/logging/xlog.h>
#include <functional>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

#include "common/app/ClientId.h"
#include "common/app/NodeId.h"
#include "common/serde/Serde.h"
#include "common/serde/Service.h"
#include "common/utils/Result.h"
#include "common/utils/Status.h"
#include "common/utils/StrongType.h"
#include "common/utils/Uuid.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/mgmtd/MgmtdTypes.h"

#define META_SERVICE_VERSION 1

#define CHECK_SESSION(session)             \
  do {                                     \
    if (!session.has_value()) {            \
      return INVALID(#session " not set"); \
    }                                      \
    if (!session->valid()) {               \
      return INVALID(#session " invalid"); \
    }                                      \
  } while (0)

namespace hf3fs::meta {

struct ReqBase {
  SERDE_STRUCT_FIELD(user, UserInfo{});
  SERDE_STRUCT_FIELD(client, ClientId{Uuid::zero()});
  SERDE_STRUCT_FIELD(forward, flat::NodeId(0));
  SERDE_STRUCT_FIELD(uuid, Uuid::zero());

 public:
  // set client id in unittest
  static std::optional<ClientId> &currentClientId() {
    static std::optional<ClientId> clientId;
    return clientId;
  }

  ReqBase(UserInfo user = {}, Uuid uuid = Uuid::zero())
      : user(std::move(user)),
        uuid(uuid) {
    if (currentClientId()) {
      client = *currentClientId();
    }
  }

  Result<Void> checkUuid() const {
    if (client.uuid == Uuid::zero()) return INVALID("Invalid client uuid");
    if (uuid == Uuid::zero()) return INVALID("Invalid request uuid");
    return VALID;
  }
};
struct RspBase {
  SERDE_STRUCT_FIELD(dummy, Void{});
};

// authentication
struct AuthReq : ReqBase {
  SERDE_STRUCT_FIELD(dummy, Void{});

 public:
  AuthReq() = default;
  AuthReq(UserInfo user)
      : ReqBase(std::move(user)) {}
  Result<Void> valid() const { return VALID; }
};

struct AuthRsp : RspBase {
  SERDE_STRUCT_FIELD(user, UserInfo{});

 public:
  AuthRsp() = default;
  AuthRsp(UserInfo user)
      : user(std::move(user)) {}
};

// statFs
struct StatFsReq : ReqBase {
 public:
  StatFsReq() = default;
  StatFsReq(UserInfo user)
      : ReqBase(std::move(user)) {}
  Result<Void> valid() const { return VALID; }
};
struct StatFsRsp : RspBase {
  SERDE_STRUCT_FIELD(capacity, uint64_t(0));
  SERDE_STRUCT_FIELD(used, uint64_t(0));
  SERDE_STRUCT_FIELD(free, uint64_t(0));

 public:
  StatFsRsp() = default;
  StatFsRsp(uint64_t capacity, uint64_t used, uint64_t free)
      : capacity(capacity),
        used(used),
        free(free) {}
};

// stat
struct StatReq : ReqBase {
  SERDE_STRUCT_FIELD(path, PathAt());
  SERDE_STRUCT_FIELD(flags, AtFlags());

 public:
  StatReq() = default;
  StatReq(UserInfo user, PathAt path, AtFlags flags)
      : ReqBase(std::move(user)),
        path(std::move(path)),
        flags(flags) {}
  Result<Void> valid() const { return flags.valid(); }
};
struct StatRsp : RspBase {
  SERDE_STRUCT_FIELD(stat, Inode());

 public:
  StatRsp() = default;
  StatRsp(Inode stat)
      : stat(std::move(stat)) {}
};

// batchStat & batchStatByPath
struct BatchStatReq : ReqBase {
  SERDE_STRUCT_FIELD(inodeIds, std::vector<InodeId>());

 public:
  BatchStatReq() = default;
  BatchStatReq(UserInfo user, std::vector<InodeId> inodeIds)
      : ReqBase(std::move(user)),
        inodeIds(std::move(inodeIds)) {}

  Result<Void> valid() const { return VALID; }
};

struct BatchStatRsp : RspBase {
  SERDE_STRUCT_FIELD(inodes, std::vector<std::optional<Inode>>());

 public:
  BatchStatRsp() = default;
  BatchStatRsp(std::vector<std::optional<Inode>> inodes)
      : inodes(std::move(inodes)) {}
};

struct BatchStatByPathReq : ReqBase {
  SERDE_STRUCT_FIELD(paths, std::vector<PathAt>());
  SERDE_STRUCT_FIELD(flags, AtFlags());

 public:
  BatchStatByPathReq() = default;
  BatchStatByPathReq(UserInfo user, std::vector<PathAt> paths, AtFlags flags)
      : ReqBase(std::move(user)),
        paths(std::move(paths)),
        flags(flags) {}

  Result<Void> valid() const { return flags.valid(); }
};

struct BatchStatByPathRsp : RspBase {
  SERDE_STRUCT_FIELD(inodes, std::vector<Result<Inode>>());

 public:
  BatchStatByPathRsp() = default;
  BatchStatByPathRsp(std::vector<Result<Inode>> inodes)
      : inodes(std::move(inodes)) {}
};

// create
struct CreateReq : ReqBase {
  SERDE_STRUCT_FIELD(path, PathAt());
  SERDE_STRUCT_FIELD(session, std::optional<SessionInfo>());
  SERDE_STRUCT_FIELD(flags, OpenFlags());
  SERDE_STRUCT_FIELD(perm, Permission());
  SERDE_STRUCT_FIELD(layout, std::optional<Layout>());
  SERDE_STRUCT_FIELD(removeChunksBatchSize, uint32_t(0));
  SERDE_STRUCT_FIELD(dynStripe, false);

 public:
  CreateReq() = default;
  CreateReq(UserInfo user,
            PathAt path,
            std::optional<SessionInfo> session,
            OpenFlags flags,
            Permission perm,
            std::optional<Layout> layout = std::nullopt,
            bool dynStripe = false)
      : ReqBase(std::move(user), Uuid::random()),
        path(std::move(path)),
        session(session),
        flags(flags),
        perm(perm),
        layout(std::move(layout)),
        removeChunksBatchSize(32),
        dynStripe(dynStripe) {}

  Result<Void> valid() const {
    RETURN_ON_ERROR(path.validForCreate());
    RETURN_ON_ERROR(flags.valid());
    if (flags.accessType() != AccessType::READ) CHECK_SESSION(session);
    if (layout.has_value()) RETURN_ON_ERROR(layout->valid(true));
    if (flags.contains(O_TRUNC) && removeChunksBatchSize == 0) return INVALID("removeChunksBatchSize == 0");
    return VALID;
  }
};
struct CreateRsp : RspBase {
  SERDE_STRUCT_FIELD(stat, Inode());
  SERDE_STRUCT_FIELD(needTruncate, false);

 public:
  CreateRsp() = default;
  CreateRsp(Inode stat, bool needTruncate)
      : stat(std::move(stat)),
        needTruncate(needTruncate) {}
};

// mkdirs
struct MkdirsReq : ReqBase {
  SERDE_STRUCT_FIELD(path, PathAt());
  SERDE_STRUCT_FIELD(perm, Permission());
  SERDE_STRUCT_FIELD(recursive, false);
  SERDE_STRUCT_FIELD(layout, std::optional<Layout>());

 public:
  MkdirsReq() = default;
  MkdirsReq(UserInfo user, PathAt path, Permission perm, bool recursive, std::optional<Layout> layout = std::nullopt)
      : ReqBase(std::move(user), Uuid::random()),
        path(std::move(path)),
        perm(perm),
        recursive(recursive),
        layout(std::move(layout)) {}
  Result<Void> valid() const {
    if (!path.path.has_value() || path.path->empty()) return INVALID("path not set");
    if (layout.has_value()) RETURN_ON_ERROR(layout->valid(true));
    return Void{};
  }
};

struct MkdirsRsp : RspBase {
  SERDE_STRUCT_FIELD(stat, Inode());

 public:
  MkdirsRsp() = default;
  MkdirsRsp(Inode stat)
      : stat(std::move(stat)) {}
};

// symlink
struct SymlinkReq : ReqBase {
  SERDE_STRUCT_FIELD(path, PathAt());
  SERDE_STRUCT_FIELD(target, Path());

 public:
  SymlinkReq() = default;
  SymlinkReq(UserInfo user, PathAt path, Path target)
      : ReqBase(std::move(user), Uuid::random()),
        path(std::move(path)),
        target(std::move(target)) {}
  Result<Void> valid() const { return path.validForCreate(); }
};
struct SymlinkRsp : RspBase {
  SERDE_STRUCT_FIELD(stat, Inode());

 public:
  SymlinkRsp() = default;
  SymlinkRsp(Inode stat)
      : stat(std::move(stat)) {}
};

// hardlink
struct HardLinkReq : ReqBase {
  SERDE_STRUCT_FIELD(oldPath, PathAt());
  SERDE_STRUCT_FIELD(newPath, PathAt());
  SERDE_STRUCT_FIELD(flags, AtFlags());

 public:
  HardLinkReq() = default;
  HardLinkReq(UserInfo user, PathAt oldPath, PathAt newPath, AtFlags flags)
      : ReqBase(std::move(user), Uuid::random()),
        oldPath(std::move(oldPath)),
        newPath(std::move(newPath)),
        flags(flags) {}
  Result<Void> valid() const {
    RETURN_ON_ERROR(newPath.validForCreate());
    RETURN_ON_ERROR(flags.valid());
    return VALID;
  }
};
struct HardLinkRsp : RspBase {
  SERDE_STRUCT_FIELD(stat, Inode());

 public:
  HardLinkRsp() = default;
  HardLinkRsp(Inode stat)
      : stat(std::move(stat)) {}
};

// remove
struct RemoveReq : ReqBase {
  SERDE_STRUCT_FIELD(path, PathAt());
  SERDE_STRUCT_FIELD(atFlags, AtFlags());
  SERDE_STRUCT_FIELD(recursive, false);
  SERDE_STRUCT_FIELD(checkType, false);
  SERDE_STRUCT_FIELD(inodeId, std::optional<InodeId>());

 public:
  RemoveReq() = default;
  RemoveReq(UserInfo user,
            PathAt path,
            AtFlags flags,
            bool recursive,
            bool checkType = false,
            std::optional<InodeId> inodeId = std::nullopt)
      : ReqBase(std::move(user), Uuid::random()),
        path(std::move(path)),
        atFlags(flags),
        recursive(recursive),
        checkType(checkType),
        inodeId(inodeId) {}
  Result<Void> valid() const {
    if (path.path.has_value()) RETURN_ON_ERROR(path.validForCreate());
    if (recursive) RETURN_ON_ERROR(checkUuid());
    return VALID;
  }
};

struct RemoveRsp : RspBase {
  SERDE_STRUCT_FIELD(dummy, Void{});
};

// open
struct OpenReq : ReqBase {
  SERDE_STRUCT_FIELD(path, PathAt());
  SERDE_STRUCT_FIELD(session, std::optional<SessionInfo>());
  SERDE_STRUCT_FIELD(flags, OpenFlags());
  SERDE_STRUCT_FIELD(removeChunksBatchSize, uint32_t(0));
  SERDE_STRUCT_FIELD(dynStripe, false);

 public:
  OpenReq() = default;
  OpenReq(UserInfo user, PathAt path, std::optional<SessionInfo> session, OpenFlags flags, bool dynStripe = false)
      : ReqBase(std::move(user)),
        path(std::move(path)),
        session(session),
        flags(flags),
        removeChunksBatchSize(32),
        dynStripe(dynStripe) {}
  Result<Void> valid() const {
    RETURN_ON_ERROR(flags.valid());
    if (flags.accessType() != AccessType::READ) CHECK_SESSION(session);
    if (flags.accessType() == AccessType::READ && (flags.contains(O_TRUNC) || flags.contains(O_APPEND)))
      return INVALID("O_RDONLY with O_TRUNC or O_APPEND");
    if (flags.contains(O_TRUNC) && removeChunksBatchSize == 0) return INVALID("removeChunksBatchSize == 0");
    return VALID;
  }
};
struct OpenRsp : RspBase {
  SERDE_STRUCT_FIELD(_unused, (uint32_t)0);
  SERDE_STRUCT_FIELD(stat, Inode());
  SERDE_STRUCT_FIELD(needTruncate, false);

 public:
  OpenRsp() = default;
  OpenRsp(Inode stat, bool needTruncate)
      : stat(std::move(stat)),
        needTruncate(needTruncate) {}
};

// sync
struct SyncReq : ReqBase {
  SERDE_STRUCT_FIELD(inode, InodeId());
  SERDE_STRUCT_FIELD(updateLength, false);
  SERDE_STRUCT_FIELD(atime, std::optional<UtcTime>());
  SERDE_STRUCT_FIELD(mtime, std::optional<UtcTime>());
  SERDE_STRUCT_FIELD(truncated, false);
  SERDE_STRUCT_FIELD(lengthHint, std::optional<VersionedLength>());

 public:
  SyncReq() = default;
  SyncReq(UserInfo user,
          InodeId inode,
          bool updateLength,
          std::optional<UtcTime> atime,
          std::optional<UtcTime> mtime,
          bool truncated = false,
          std::optional<VersionedLength> hint = std::nullopt)
      : ReqBase(std::move(user)),
        inode(inode),
        updateLength(updateLength),
        atime(atime),
        mtime(mtime),
        truncated(truncated),
        lengthHint(hint) {}
  Result<Void> valid() const {
    if (truncated && !updateLength) return INVALID("truncate but not updateLength");
    return VALID;
  }
};
struct SyncRsp : RspBase {
  SERDE_STRUCT_FIELD(_unused, (uint32_t)0);
  SERDE_STRUCT_FIELD(stat, Inode());

 public:
  SyncRsp() = default;
  SyncRsp(Inode stat)
      : stat(std::move(stat)) {}
};

// close
struct CloseReq : ReqBase {
  SERDE_STRUCT_FIELD(inode, InodeId());
  SERDE_STRUCT_FIELD(session, std::optional<SessionInfo>());
  SERDE_STRUCT_FIELD(updateLength, false);
  SERDE_STRUCT_FIELD(atime, std::optional<UtcTime>());
  SERDE_STRUCT_FIELD(mtime, std::optional<UtcTime>());
  SERDE_STRUCT_FIELD(lengthHint, std::optional<VersionedLength>());

 public:
  bool pruneSession = false;

  CloseReq() = default;
  CloseReq(UserInfo user,
           InodeId inode,
           std::optional<SessionInfo> session,
           bool updateLength,
           std::optional<UtcTime> atime,
           std::optional<UtcTime> mtime)
      : ReqBase(std::move(user)),
        inode(inode),
        session(session),
        updateLength(updateLength),
        atime(atime),
        mtime(mtime) {}
  Result<Void> valid() const {
    if (updateLength || mtime.has_value()) CHECK_SESSION(session);
    return VALID;
  }
};
struct CloseRsp : RspBase {
  SERDE_STRUCT_FIELD(_unused, (uint32_t)0);
  SERDE_STRUCT_FIELD(stat, Inode());

 public:
  CloseRsp() = default;
  CloseRsp(Inode stat)
      : stat(std::move(stat)) {}
};

// rename
struct RenameReq : ReqBase {
  SERDE_STRUCT_FIELD(src, PathAt());
  SERDE_STRUCT_FIELD(dest, PathAt());
  SERDE_STRUCT_FIELD(moveToTrash, false);
  SERDE_STRUCT_FIELD(inodeId, std::optional<InodeId>());

 public:
  RenameReq() = default;
  RenameReq(UserInfo user,
            PathAt src,
            PathAt dest,
            bool moveToTrash = false,
            std::optional<InodeId> inodeId = std::nullopt)
      : ReqBase(std::move(user), Uuid::random()),
        src(std::move(src)),
        dest(std::move(dest)),
        moveToTrash(moveToTrash),
        inodeId(inodeId) {}
  Result<Void> valid() const {
    RETURN_ON_ERROR(src.validForCreate());
    RETURN_ON_ERROR(dest.validForCreate());
    if (moveToTrash) RETURN_ON_ERROR(checkUuid());
    return VALID;
  }
};
struct RenameRsp : RspBase {
  SERDE_STRUCT_FIELD(dummy, Void{});
  SERDE_STRUCT_FIELD(stat, std::optional<Inode>());

 public:
  RenameRsp() = default;
  RenameRsp(Inode stat)
      : stat(std::move(stat)) {}
};

// list
struct ListReq : ReqBase {
  SERDE_STRUCT_FIELD(path, PathAt());
  SERDE_STRUCT_FIELD(prev, std::string());
  SERDE_STRUCT_FIELD(limit, (int32_t)-1);
  SERDE_STRUCT_FIELD(status, false);

 public:
  ListReq() = default;
  ListReq(UserInfo user, PathAt path, std::string prev = {}, int32_t limit = -1, bool status = false)
      : ReqBase(std::move(user)),
        path(std::move(path)),
        prev(std::move(prev)),
        limit(limit),
        status(status) {}
  Result<Void> valid() const { return VALID; }
};
struct ListRsp : RspBase {
  SERDE_STRUCT_FIELD(entries, std::vector<DirEntry>());
  SERDE_STRUCT_FIELD(inodes, std::vector<Inode>());
  SERDE_STRUCT_FIELD(more, false);

 public:
  ListRsp() = default;
  ListRsp(std::vector<DirEntry> entries, std::vector<Inode> inodes, bool more)
      : entries(std::move(entries)),
        inodes(std::move(inodes)),
        more(more) {}
};

// truncate
struct TruncateReq : ReqBase {
  SERDE_STRUCT_FIELD(inode, InodeId(0));
  SERDE_STRUCT_FIELD(length, uint64_t(0));
  SERDE_STRUCT_FIELD(removeChunksBatchSize, uint32_t(0));

 public:
  TruncateReq() = default;
  TruncateReq(UserInfo user, InodeId inode, uint64_t length, uint32_t removeChunksBatchSize)
      : ReqBase(std::move(user)),
        inode(inode),
        length(length),
        removeChunksBatchSize(removeChunksBatchSize) {}
  Result<Void> valid() const {
    if (removeChunksBatchSize == 0) return INVALID("removeChunksBatchSize == 0");
    return VALID;
  }
};
struct TruncateRsp : RspBase {
  SERDE_STRUCT_FIELD(chunksRemoved, uint32_t(0));
  SERDE_STRUCT_FIELD(stat, Inode());
  SERDE_STRUCT_FIELD(finished, true);

 public:
  TruncateRsp() = default;
  TruncateRsp(Inode stat)
      : TruncateRsp(std::move(stat), 0, true) {}
  TruncateRsp(Inode stat, uint32_t chunksRemoved, bool finished)
      : chunksRemoved(chunksRemoved),
        stat(std::move(stat)),
        finished(finished) {}
};

// getRealPath
struct GetRealPathReq : ReqBase {
  SERDE_STRUCT_FIELD(path, PathAt());
  SERDE_STRUCT_FIELD(absolute, false);

 public:
  GetRealPathReq() = default;
  GetRealPathReq(UserInfo user, PathAt path, bool absolute)
      : ReqBase(std::move(user)),
        path(std::move(path)),
        absolute(absolute) {}
  Result<Void> valid() const { return VALID; }
};
struct GetRealPathRsp : RspBase {
  SERDE_STRUCT_FIELD(path, Path());

 public:
  GetRealPathRsp() = default;
  GetRealPathRsp(Path path)
      : path(std::move(path)) {}
};

// setAttr
struct SetAttrReq : ReqBase {
  SERDE_STRUCT_FIELD(path, PathAt());
  SERDE_STRUCT_FIELD(flags, AtFlags());
  SERDE_STRUCT_FIELD(uid, std::optional<Uid>());
  SERDE_STRUCT_FIELD(gid, std::optional<Gid>());
  SERDE_STRUCT_FIELD(perm, std::optional<Permission>());
  SERDE_STRUCT_FIELD(atime, std::optional<UtcTime>());
  SERDE_STRUCT_FIELD(mtime, std::optional<UtcTime>());
  SERDE_STRUCT_FIELD(layout, std::optional<Layout>());
  SERDE_STRUCT_FIELD(iflags, std::optional<IFlags>());
  SERDE_STRUCT_FIELD(dynStripe, uint32_t(0));

 public:
  static SetAttrReq setLayout(UserInfo user, PathAt path, AtFlags flags, Layout layout) {
    return {std::move(user), std::move(path), flags, {}, {}, {}, {}, {}, std::move(layout)};
  }
  static SetAttrReq setPermission(UserInfo user,
                                  PathAt path,
                                  AtFlags flags,
                                  std::optional<Uid> uid,
                                  std::optional<Gid> gid,
                                  std::optional<Permission> perm,
                                  std::optional<IFlags> iflags = std::nullopt) {
    return {std::move(user), std::move(path), flags, uid, gid, perm, {}, {}, {}, iflags};
  }
  static SetAttrReq setIFlags(UserInfo user, PathAt path, IFlags iflags) {
    return {std::move(user), std::move(path), AtFlags(), {}, {}, {}, {}, {}, {}, iflags};
  }
  static SetAttrReq utimes(UserInfo user,
                           PathAt path,
                           AtFlags flags,
                           std::optional<UtcTime> atime,
                           std::optional<UtcTime> mtime) {
    return {std::move(user), std::move(path), flags, {}, {}, {}, atime, mtime, {}};
  }
  static SetAttrReq extendStripe(UserInfo user, InodeId inode, uint32_t stripe) {
    return {std::move(user), PathAt(inode), AtFlags{}, {}, {}, {}, {}, {}, {}, {}, stripe};
  }
  Result<Void> valid() const {
    if (layout.has_value()) RETURN_ON_ERROR(layout->valid(true));
    if (iflags && (*iflags & ~FS_FL_SUPPORTED)) {
      return MAKE_ERROR_F(StatusCode::kInvalidArg, "only support {:x}", (uint32_t)FS_FL_SUPPORTED);
    }
    return VALID;
  }
};

struct SetAttrRsp : RspBase {
  SERDE_STRUCT_FIELD(stat, Inode());

 public:
  SetAttrRsp() = default;
  SetAttrRsp(Inode stat)
      : stat(std::move(stat)) {}
};

// pruneSession
struct PruneSessionReq : ReqBase {
  SERDE_STRUCT_FIELD(client, ClientId::zero());
  SERDE_STRUCT_FIELD(sessions, std::vector<Uuid>());
  SERDE_STRUCT_FIELD(needSync, std::vector<bool>());  // deperated

 public:
  PruneSessionReq() = default;
  PruneSessionReq(ClientId client, std::vector<Uuid> sessions)
      : ReqBase(),
        client(client),
        sessions(std::move(sessions)) {}
  Result<Void> valid() const { return VALID; }
};
struct PruneSessionRsp : RspBase {
  SERDE_STRUCT_FIELD(dummy, Void{});
};

// dropUserCache
struct DropUserCacheReq : ReqBase {
  SERDE_STRUCT_FIELD(uid, std::optional<Uid>());
  SERDE_STRUCT_FIELD(dropAll, false);

 public:
  DropUserCacheReq() = default;
  DropUserCacheReq(std::optional<Uid> u, bool d)
      : uid(std::move(u)),
        dropAll(d) {}
  Result<Void> valid() const { return VALID; }
};
struct DropUserCacheRsp : RspBase {
  SERDE_STRUCT_FIELD(dummy, Void{});
};

// lockDirectory
struct LockDirectoryReq : ReqBase {
  enum class LockAction : uint8_t {
    TryLock,
    PreemptLock,
    UnLock,
    Clear,
  };
  SERDE_STRUCT_FIELD(inode, InodeId());
  SERDE_STRUCT_FIELD(action, LockAction::TryLock);

 public:
  LockDirectoryReq() = default;
  LockDirectoryReq(UserInfo user, InodeId inode, LockAction action)
      : ReqBase(user),
        inode(inode),
        action(action) {}
  Result<Void> valid() const { return VALID; }
};

struct LockDirectoryRsp : RspBase {
  SERDE_STRUCT_FIELD(dummy, Void{});

 public:
  LockDirectoryRsp() = default;
};

// testRpc
struct TestRpcReq : ReqBase {
  SERDE_STRUCT_FIELD(path, PathAt());
  SERDE_STRUCT_FIELD(flags, uint32_t(0));

 public:
  Result<Void> valid() const { return VALID; }
};
struct TestRpcRsp : RspBase {
  SERDE_STRUCT_FIELD(stat, Inode());
};

/* MetaSerde service */
SERDE_SERVICE(MetaSerde, 4) {
#define META_SERVICE_METHOD(NAME, CODE, REQ, RSP)                                   \
  SERDE_SERVICE_METHOD(NAME, CODE, REQ, RSP);                                       \
                                                                                    \
 public:                                                                            \
  static constexpr std::string_view getRpcName(const REQ &) { return #NAME; }       \
  static_assert(serde::SerializableToBytes<REQ> && serde::SerializableToJson<REQ>); \
  static_assert(serde::SerializableToBytes<RSP> && serde::SerializableToJson<RSP>)

  META_SERVICE_METHOD(statFs, 1, StatFsReq, StatFsRsp);
  META_SERVICE_METHOD(stat, 2, StatReq, StatRsp);
  META_SERVICE_METHOD(create, 3, CreateReq, CreateRsp);
  META_SERVICE_METHOD(mkdirs, 4, MkdirsReq, MkdirsRsp);
  META_SERVICE_METHOD(symlink, 5, SymlinkReq, SymlinkRsp);
  META_SERVICE_METHOD(hardLink, 6, HardLinkReq, HardLinkRsp);
  META_SERVICE_METHOD(remove, 7, RemoveReq, RemoveRsp);
  META_SERVICE_METHOD(open, 8, OpenReq, OpenRsp);
  META_SERVICE_METHOD(sync, 9, SyncReq, SyncRsp);
  META_SERVICE_METHOD(close, 10, CloseReq, CloseRsp);
  META_SERVICE_METHOD(rename, 11, RenameReq, RenameRsp);
  META_SERVICE_METHOD(list, 12, ListReq, ListRsp);
  // deperated:
  META_SERVICE_METHOD(truncate, 13, TruncateReq, TruncateRsp);
  META_SERVICE_METHOD(getRealPath, 14, GetRealPathReq, GetRealPathRsp);
  META_SERVICE_METHOD(setAttr, 15, SetAttrReq, SetAttrRsp);
  META_SERVICE_METHOD(pruneSession, 16, PruneSessionReq, PruneSessionRsp);
  META_SERVICE_METHOD(dropUserCache, 17, DropUserCacheReq, DropUserCacheRsp);
  META_SERVICE_METHOD(authenticate, 18, AuthReq, AuthRsp);
  META_SERVICE_METHOD(lockDirectory, 19, LockDirectoryReq, LockDirectoryRsp);
  META_SERVICE_METHOD(batchStat, 20, BatchStatReq, BatchStatRsp);
  META_SERVICE_METHOD(batchStatByPath, 21, BatchStatByPathReq, BatchStatByPathRsp);

  META_SERVICE_METHOD(testRpc, 50, TestRpcReq, TestRpcRsp);

#undef META_SERVICE_METHOD
};

}  // namespace hf3fs::meta
