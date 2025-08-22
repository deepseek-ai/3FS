#pragma once

#include "common/serde/CallContext.h"
#include "common/serde/Service.h"
#include "common/utils/Result.h"
#include "fbs/meta/Service.h"

namespace hf3fs::meta {

class MockMetaService : public serde::ServiceWrapper<MockMetaService, MetaSerde> {
 public:
  virtual ~MockMetaService() = default;

#define META_MOCK_SERVICE_METHOD(NAME, REQ, RESP)                         \
  virtual CoTryTask<RESP> NAME(serde::CallContext &ctx, const REQ &req) { \
    co_return makeError(StatusCode::kNotImplemented);                     \
  }

  META_MOCK_SERVICE_METHOD(statFs, StatFsReq, StatFsRsp);
  META_MOCK_SERVICE_METHOD(stat, StatReq, StatRsp);
  META_MOCK_SERVICE_METHOD(create, CreateReq, CreateRsp);
  META_MOCK_SERVICE_METHOD(mkdirs, MkdirsReq, MkdirsRsp);
  META_MOCK_SERVICE_METHOD(symlink, SymlinkReq, SymlinkRsp);
  META_MOCK_SERVICE_METHOD(hardLink, HardLinkReq, HardLinkRsp);
  META_MOCK_SERVICE_METHOD(remove, RemoveReq, RemoveRsp);
  META_MOCK_SERVICE_METHOD(open, OpenReq, OpenRsp);
  META_MOCK_SERVICE_METHOD(sync, SyncReq, SyncRsp);
  META_MOCK_SERVICE_METHOD(close, CloseReq, CloseRsp);
  META_MOCK_SERVICE_METHOD(rename, RenameReq, RenameRsp);
  META_MOCK_SERVICE_METHOD(list, ListReq, ListRsp);
  META_MOCK_SERVICE_METHOD(truncate, TruncateReq, TruncateRsp);
  META_MOCK_SERVICE_METHOD(getRealPath, GetRealPathReq, GetRealPathRsp);
  META_MOCK_SERVICE_METHOD(setAttr, SetAttrReq, SetAttrRsp);
  META_MOCK_SERVICE_METHOD(pruneSession, PruneSessionReq, PruneSessionRsp);
  META_MOCK_SERVICE_METHOD(dropUserCache, DropUserCacheReq, DropUserCacheRsp);
  META_MOCK_SERVICE_METHOD(authenticate, AuthReq, AuthRsp);
  META_MOCK_SERVICE_METHOD(lockDirectory, LockDirectoryReq, LockDirectoryRsp);
  META_MOCK_SERVICE_METHOD(testRpc, TestRpcReq, TestRpcRsp);
  META_MOCK_SERVICE_METHOD(batchStat, BatchStatReq, BatchStatRsp);
  META_MOCK_SERVICE_METHOD(batchStatByPath, BatchStatByPathReq, BatchStatByPathRsp);
};
}  // namespace hf3fs::meta
