#pragma once

#include "common/serde/ClientContext.h"
#include "fbs/meta/Service.h"
#include "stubs/MetaService/IMetaServiceStub.h"

namespace hf3fs::meta {

template <typename Ctx>
class MetaServiceStub : public IMetaServiceStub {
 public:
  explicit MetaServiceStub(Ctx ctx)
      : context_(std::move(ctx)) {}

#define META_STUB_METHOD(NAME, REQ, RESP)                                                                             \
  CoTryTask<RESP> NAME(const REQ &req, const net::UserRequestOptions &options, serde::Timestamp *timestamp = nullptr) \
      override

  META_STUB_METHOD(statFs, StatFsReq, StatFsRsp);
  META_STUB_METHOD(stat, StatReq, StatRsp);
  META_STUB_METHOD(create, CreateReq, CreateRsp);
  META_STUB_METHOD(mkdirs, MkdirsReq, MkdirsRsp);
  META_STUB_METHOD(symlink, SymlinkReq, SymlinkRsp);
  META_STUB_METHOD(hardLink, HardLinkReq, HardLinkRsp);
  META_STUB_METHOD(remove, RemoveReq, RemoveRsp);
  META_STUB_METHOD(open, OpenReq, OpenRsp);
  META_STUB_METHOD(sync, SyncReq, SyncRsp);
  META_STUB_METHOD(close, CloseReq, CloseRsp);
  META_STUB_METHOD(rename, RenameReq, RenameRsp);
  META_STUB_METHOD(list, ListReq, ListRsp);
  META_STUB_METHOD(truncate, TruncateReq, TruncateRsp);
  META_STUB_METHOD(getRealPath, GetRealPathReq, GetRealPathRsp);
  META_STUB_METHOD(setAttr, SetAttrReq, SetAttrRsp);
  META_STUB_METHOD(pruneSession, PruneSessionReq, PruneSessionRsp);
  META_STUB_METHOD(dropUserCache, DropUserCacheReq, DropUserCacheRsp);
  META_STUB_METHOD(authenticate, AuthReq, AuthRsp);
  META_STUB_METHOD(lockDirectory, LockDirectoryReq, LockDirectoryRsp);
  META_STUB_METHOD(testRpc, TestRpcReq, TestRpcRsp);
  META_STUB_METHOD(batchStat, BatchStatReq, BatchStatRsp);
  META_STUB_METHOD(batchStatByPath, BatchStatByPathReq, BatchStatByPathRsp);

#undef META_STUB_METHOD

 private:
  Ctx context_;
};

}  // namespace hf3fs::meta
