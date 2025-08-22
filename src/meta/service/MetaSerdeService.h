#pragma once

#include "common/serde/CallContext.h"
#include "fbs/meta/Service.h"
#include "meta/service/MetaOperator.h"

namespace hf3fs::meta::server {

class MetaSerdeService : public serde::ServiceWrapper<MetaSerdeService, MetaSerde> {
 public:
  MetaSerdeService(MetaOperator &meta)
      : meta_(meta) {}

#define META_SERVICE_METHOD(NAME, REQ, RESP) \
  CoTryTask<RESP> NAME(serde::CallContext &, const REQ &req) { return meta_.NAME(req); }

  META_SERVICE_METHOD(statFs, StatFsReq, StatFsRsp);
  META_SERVICE_METHOD(stat, StatReq, StatRsp);
  META_SERVICE_METHOD(create, CreateReq, CreateRsp);
  META_SERVICE_METHOD(mkdirs, MkdirsReq, MkdirsRsp);
  META_SERVICE_METHOD(symlink, SymlinkReq, SymlinkRsp);
  META_SERVICE_METHOD(hardLink, HardLinkReq, HardLinkRsp);
  META_SERVICE_METHOD(remove, RemoveReq, RemoveRsp);
  META_SERVICE_METHOD(open, OpenReq, OpenRsp);
  META_SERVICE_METHOD(sync, SyncReq, SyncRsp);
  META_SERVICE_METHOD(close, CloseReq, CloseRsp);
  META_SERVICE_METHOD(rename, RenameReq, RenameRsp);
  META_SERVICE_METHOD(list, ListReq, ListRsp);
  META_SERVICE_METHOD(truncate, TruncateReq, TruncateRsp);
  META_SERVICE_METHOD(getRealPath, GetRealPathReq, GetRealPathRsp);
  META_SERVICE_METHOD(setAttr, SetAttrReq, SetAttrRsp);
  META_SERVICE_METHOD(pruneSession, PruneSessionReq, PruneSessionRsp);
  META_SERVICE_METHOD(dropUserCache, DropUserCacheReq, DropUserCacheRsp);
  META_SERVICE_METHOD(authenticate, AuthReq, AuthRsp);
  META_SERVICE_METHOD(lockDirectory, LockDirectoryReq, LockDirectoryRsp);
  META_SERVICE_METHOD(testRpc, TestRpcReq, TestRpcRsp);
  META_SERVICE_METHOD(batchStat, BatchStatReq, BatchStatRsp);
  META_SERVICE_METHOD(batchStatByPath, BatchStatByPathReq, BatchStatByPathRsp);
#undef META_SERVICE_METHOD

 private:
  MetaOperator &meta_;
};

}  // namespace hf3fs::meta::server
