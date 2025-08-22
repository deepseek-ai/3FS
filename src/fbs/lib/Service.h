#pragma once

#include "Schema.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"

namespace hf3fs::lib::agent {

using hf3fs::Result;

template <typename ValType>
struct OneValRsp {
  SERDE_STRUCT_TYPED_FIELD(ValType, val, ValType{});
};
using EmptyRsp = OneValRsp<Void>;

struct ProcUser {
  SERDE_STRUCT_TYPED_FIELD(ProcessInfo, proc, ProcessInfo{});
  SERDE_STRUCT_TYPED_FIELD(flat::UserInfo, userInfo, flat::UserInfo{});
};

template <bool OptionalPath>
struct PathAt {
  using PathType = std::conditional_t<OptionalPath, std::optional<Path>, Path>;
  SERDE_STRUCT_TYPED_FIELD(int, dirfd, 0);
  SERDE_STRUCT_TYPED_FIELD(PathType, path, PathType{});
};

struct StatFsReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<true>, pathAt, PathAt<true>{});
};
using StatFsRsp = OneValRsp<StatFs>;

struct StatReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<true>, pathAt, PathAt<true>{});
  SERDE_STRUCT_TYPED_FIELD(bool, followLastSymlink, false);
  SERDE_STRUCT_TYPED_FIELD(bool, ignoreCache, false);
};
using StatRsp = OneValRsp<Stat>;

struct OpenReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<false>, pathAt, PathAt<false>{});
  SERDE_STRUCT_TYPED_FIELD(int, flags, 0);
  SERDE_STRUCT_TYPED_FIELD(meta::Permission, mode, meta::Permission{});
};
using OpenRsp = OneValRsp<uint>;

struct CloseReq {
  SERDE_STRUCT_TYPED_FIELD(ProcessInfo, proc, ProcessInfo{});
  SERDE_STRUCT_TYPED_FIELD(int, fd, 0);
};
using CloseRsp = EmptyRsp;

struct SetAttrsReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<true>, pathAt, PathAt<true>{});
  SERDE_STRUCT_TYPED_FIELD(bool, followLastSymlink, false);
  SERDE_STRUCT_TYPED_FIELD(std::optional<flat::Uid>, uid, std::nullopt);
  SERDE_STRUCT_TYPED_FIELD(std::optional<flat::Gid>, gid, std::nullopt);
  SERDE_STRUCT_TYPED_FIELD(std::optional<meta::Permission>, perm, std::nullopt);
  SERDE_STRUCT_TYPED_FIELD(std::optional<UtcTime>, atime, std::nullopt);
  SERDE_STRUCT_TYPED_FIELD(std::optional<UtcTime>, mtime, std::nullopt);
};
using SetAttrsRsp = EmptyRsp;

struct DupReq {
  SERDE_STRUCT_TYPED_FIELD(ProcessInfo, proc, ProcessInfo{});
  SERDE_STRUCT_TYPED_FIELD(int, srcfd, 0);
  SERDE_STRUCT_TYPED_FIELD(int, dstfd, 0);
  SERDE_STRUCT_TYPED_FIELD(int, flags, 0);
};
using DupRsp = OneValRsp<uint>;

struct MkdirsReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<false>, pathAt, PathAt<false>{});
  SERDE_STRUCT_TYPED_FIELD(meta::Permission, perm, meta::Permission{0});
  SERDE_STRUCT_TYPED_FIELD(bool, recursive, false);
};
using MkdirsRsp = EmptyRsp;

struct SymlinkReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<false>, pathAt, PathAt<false>{});
  SERDE_STRUCT_TYPED_FIELD(Path, target, Path{});
};
using SymlinkRsp = EmptyRsp;

struct RemoveReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<false>, pathAt, PathAt<false>{});
  SERDE_STRUCT_TYPED_FIELD(bool, recursive, false);
  SERDE_STRUCT_TYPED_FIELD(std::optional<bool>, isDir, std::nullopt);
};
using RemoveRsp = EmptyRsp;

struct RenameReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<false>, srcPathAt, PathAt<false>{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<false>, dstPathAt, PathAt<false>{});
};
using RenameRsp = EmptyRsp;

struct ListReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<true>, pathAt, PathAt<true>{});
  SERDE_STRUCT_TYPED_FIELD(std::optional<String>, prev, std::nullopt);
};
using ListRsp = OneValRsp<DirEntList>;

struct GetRealPathReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<false>, pathAt, PathAt<false>{});
  SERDE_STRUCT_TYPED_FIELD(bool, absolute, false);
};
using GetRealPathRsp = OneValRsp<Path>;

struct FsyncReq {
  SERDE_STRUCT_TYPED_FIELD(ProcessInfo, proc, ProcessInfo{});
  SERDE_STRUCT_TYPED_FIELD(int, fd, 0);
};
using FsyncRsp = EmptyRsp;

struct FtruncateReq {
  SERDE_STRUCT_TYPED_FIELD(ProcessInfo, proc, ProcessInfo{});
  SERDE_STRUCT_TYPED_FIELD(int, fd, 0);
  SERDE_STRUCT_TYPED_FIELD(long, length, 0);
};
using FtruncateRsp = EmptyRsp;

struct LinkReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<false>, oldPathAt, PathAt<false>{});
  SERDE_STRUCT_TYPED_FIELD(PathAt<false>, newPathAt, PathAt<false>{});
  SERDE_STRUCT_TYPED_FIELD(bool, followLastSymlink, false);
};
using LinkRsp = EmptyRsp;

struct IovAllocReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(ulong, bytes, 0);
  SERDE_STRUCT_TYPED_FIELD(std::optional<int>, numa, std::nullopt);
  SERDE_STRUCT_TYPED_FIELD(bool, global, false);
  SERDE_STRUCT_TYPED_FIELD(ulong, blockSize, 0);
};
using IovAllocRsp = OneValRsp<AllocatedIov>;

struct IovFreeReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(int, iovDesc, 0);
  SERDE_STRUCT_TYPED_FIELD(Uuid, iovId, Uuid{});
  SERDE_STRUCT_TYPED_FIELD(long, offInBuf, 0);
  SERDE_STRUCT_TYPED_FIELD(ulong, bytes, 0);
};
using IovFreeRsp = EmptyRsp;

struct SeekInFileReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(int, fd, 0);
  SERDE_STRUCT_TYPED_FIELD(long, offset, 0);
  SERDE_STRUCT_TYPED_FIELD(int, whence, 0);
  SERDE_STRUCT_TYPED_FIELD(ulong, readahead, 0);
};
using SeekInFileRsp = OneValRsp<long>;

struct PioVReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(bool, read, false);
  SERDE_STRUCT_TYPED_FIELD(std::vector<IoInfo>, iov, std::vector<IoInfo>{});
  SERDE_STRUCT_TYPED_FIELD(bool, updateOffset, false);
};
using PioVRsp = OneValRsp<PioVRes>;

struct SharedFileHandlesReq {
  SERDE_STRUCT_TYPED_FIELD(ProcessInfo, proc, ProcessInfo{});
  SERDE_STRUCT_TYPED_FIELD(std::vector<int>, fds, std::vector<int>{});
};
using SharedFileHandlesRsp = OneValRsp<String>;

struct OpenWithFileHandlesReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(String, fhs, String{});
};
using OpenWithFileHandlesRsp = OneValRsp<std::vector<int>>;

struct OpenIovecHandleReq {
  SERDE_STRUCT_TYPED_FIELD(ProcUser, procUser, ProcUser{});
  SERDE_STRUCT_TYPED_FIELD(AllocatedIov, aiov, AllocatedIov{});
};
using OpenIovecHandleRsp = OneValRsp<int>;

SERDE_SERVICE(ClientAgentSerde, 10) {
  SERDE_SERVICE_METHOD(statFs, 1, StatFsReq, StatFsRsp);
  SERDE_SERVICE_METHOD(stat, 2, StatReq, StatRsp);
  SERDE_SERVICE_METHOD(open, 3, OpenReq, OpenRsp);
  SERDE_SERVICE_METHOD(close, 4, CloseReq, CloseRsp);
  SERDE_SERVICE_METHOD(setAttrs, 5, SetAttrsReq, SetAttrsRsp);
  SERDE_SERVICE_METHOD(dup, 6, DupReq, DupRsp);
  SERDE_SERVICE_METHOD(mkdirs, 7, MkdirsReq, MkdirsRsp);
  SERDE_SERVICE_METHOD(symlink, 8, SymlinkReq, SymlinkRsp);
  SERDE_SERVICE_METHOD(remove, 9, RemoveReq, RemoveRsp);
  SERDE_SERVICE_METHOD(rename, 10, RenameReq, RenameRsp);
  SERDE_SERVICE_METHOD(list, 11, ListReq, ListRsp);
  SERDE_SERVICE_METHOD(getRealPath, 12, GetRealPathReq, GetRealPathRsp);
  SERDE_SERVICE_METHOD(fsync, 13, FsyncReq, FsyncRsp);
  SERDE_SERVICE_METHOD(ftruncate, 14, FtruncateReq, FtruncateRsp);
  SERDE_SERVICE_METHOD(link, 15, LinkReq, LinkRsp);

  SERDE_SERVICE_METHOD(iovalloc, 101, IovAllocReq, IovAllocRsp);
  SERDE_SERVICE_METHOD(iovfree, 102, IovFreeReq, IovFreeRsp);
  SERDE_SERVICE_METHOD(seekInFile, 103, SeekInFileReq, SeekInFileRsp);
  SERDE_SERVICE_METHOD(piov, 104, PioVReq, PioVRsp);

  SERDE_SERVICE_METHOD(sharedFileHandles, 201, SharedFileHandlesReq, SharedFileHandlesRsp);
  SERDE_SERVICE_METHOD(openWithFileHandles, 202, OpenWithFileHandlesReq, OpenWithFileHandlesRsp);
  SERDE_SERVICE_METHOD(openIovecHandle, 203, OpenIovecHandleReq, OpenIovecHandleRsp);
};
}  // namespace hf3fs::lib::agent
