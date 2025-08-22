#include "MetaServiceStub.h"

#include "common/serde/ClientContext.h"
#include "common/serde/ClientMockContext.h"
#include "common/utils/Coroutine.h"
#include "common/utils/UtcTimeSerde.h"
#include "fbs/meta/Service.h"

#define IMPL_META_STUB_METHOD(NAME, REQ, RESP)                                           \
  template <typename Context>                                                            \
  CoTryTask<RESP> MetaServiceStub<Context>::NAME(const REQ &req,                         \
                                                 const net::UserRequestOptions &options, \
                                                 hf3fs::serde::Timestamp *timestamp) {   \
    co_return co_await MetaSerde<>::NAME<Context>(context_, req, &options, timestamp);   \
  }

namespace hf3fs::meta {

IMPL_META_STUB_METHOD(statFs, StatFsReq, StatFsRsp);
IMPL_META_STUB_METHOD(stat, StatReq, StatRsp);
IMPL_META_STUB_METHOD(create, CreateReq, CreateRsp);
IMPL_META_STUB_METHOD(mkdirs, MkdirsReq, MkdirsRsp);
IMPL_META_STUB_METHOD(symlink, SymlinkReq, SymlinkRsp);
IMPL_META_STUB_METHOD(hardLink, HardLinkReq, HardLinkRsp);
IMPL_META_STUB_METHOD(remove, RemoveReq, RemoveRsp);
IMPL_META_STUB_METHOD(open, OpenReq, OpenRsp);
IMPL_META_STUB_METHOD(sync, SyncReq, SyncRsp);
IMPL_META_STUB_METHOD(close, CloseReq, CloseRsp);
IMPL_META_STUB_METHOD(rename, RenameReq, RenameRsp);
IMPL_META_STUB_METHOD(list, ListReq, ListRsp);
IMPL_META_STUB_METHOD(truncate, TruncateReq, TruncateRsp);
IMPL_META_STUB_METHOD(getRealPath, GetRealPathReq, GetRealPathRsp);
IMPL_META_STUB_METHOD(setAttr, SetAttrReq, SetAttrRsp);
IMPL_META_STUB_METHOD(pruneSession, PruneSessionReq, PruneSessionRsp);
IMPL_META_STUB_METHOD(dropUserCache, DropUserCacheReq, DropUserCacheRsp);
IMPL_META_STUB_METHOD(authenticate, AuthReq, AuthRsp);
IMPL_META_STUB_METHOD(lockDirectory, LockDirectoryReq, LockDirectoryRsp);
IMPL_META_STUB_METHOD(testRpc, TestRpcReq, TestRpcRsp);
IMPL_META_STUB_METHOD(batchStat, BatchStatReq, BatchStatRsp);
IMPL_META_STUB_METHOD(batchStatByPath, BatchStatByPathReq, BatchStatByPathRsp);

template class MetaServiceStub<serde::ClientContext>;
template class MetaServiceStub<serde::ClientMockContext>;
}  // namespace hf3fs::meta
