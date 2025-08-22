#pragma once

#include <boost/core/ignore_unused.hpp>
#include <fcntl.h>
#include <folly/Likely.h>
#include <folly/lang/Bits.h>
#include <gtest/gtest_prod.h>
#include <memory>
#include <optional>
#include <queue>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/kv/IKVEngine.h"
#include "common/kv/ITransaction.h"
#include "common/kv/WithTransaction.h"
#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/Status.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Service.h"
#include "meta/base/Config.h"
#include "meta/components/AclCache.h"
#include "meta/components/ChainAllocator.h"
#include "meta/components/FileHelper.h"
#include "meta/components/GcManager.h"
#include "meta/components/InodeIdAllocator.h"
#include "meta/components/SessionManager.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/PathResolve.h"
#include "meta/store/Utils.h"

namespace hf3fs::meta::server {
using hf3fs::kv::IReadOnlyTransaction;
using hf3fs::kv::IReadWriteTransaction;

template <typename Rsp>
class IOperation {
 public:
  using RspT = Rsp;

  virtual ~IOperation() = default;

  virtual bool isReadOnly() = 0;
  virtual bool retryMaybeCommitted() { return true; }

  virtual bool needIdempotent(Uuid &clientId, Uuid &requestId) const {
    boost::ignore_unused(clientId, requestId);
    return false;
  }

  virtual std::string_view name() const { return "other"; }
  virtual flat::Uid user() const { return flat::Uid(-1); }

  virtual CoTryTask<Rsp> run(IReadWriteTransaction &) = 0;

  virtual void retry(const Status &) = 0;
  virtual void finish(const Result<Rsp> &) = 0;

  CoTryTask<Rsp> operator()(IReadWriteTransaction &txn) { co_return co_await run(txn); }
};

class MetaStore {
 public:
  MetaStore(const Config &config,
            analytics::StructuredTraceLog<MetaEventTrace> &metaEventTraceLog,
            std::shared_ptr<Distributor> distributor,
            std::shared_ptr<InodeIdAllocator> inodeAlloc,
            std::shared_ptr<ChainAllocator> chainAlloc,
            std::shared_ptr<FileHelper> fileHelper,
            std::shared_ptr<SessionManager> sessionManager,
            std::shared_ptr<GcManager> gcManager)
      : config_(config),
        metaEventTraceLog_(metaEventTraceLog),
        distributor_(distributor),
        inodeAlloc_(inodeAlloc),
        chainAlloc_(chainAlloc),
        fileHelper_(fileHelper),
        sessionManager_(sessionManager),
        gcManager_(gcManager),
        aclCache_(2 << 20 /* 2m acl */) {}

  auto &getEventTraceLog() { return metaEventTraceLog_; }

  template <typename Rsp>
  using Op = IOperation<Rsp>;

  template <typename Rsp>
  using OpPtr = std::unique_ptr<IOperation<Rsp>>;

  static OpPtr<Void> initFileSystem(ChainAllocator &chainAlloc, Layout rootLayout);

  OpPtr<Void> initFs(Layout rootLayout) { return MetaStore::initFileSystem(*chainAlloc_, rootLayout); }

  OpPtr<StatFsRsp> statFs(const StatFsReq &req);

  OpPtr<StatRsp> stat(const StatReq &req);

  OpPtr<BatchStatRsp> batchStat(const BatchStatReq &req);

  OpPtr<BatchStatByPathRsp> batchStatByPath(const BatchStatByPathReq &req);

  OpPtr<GetRealPathRsp> getRealPath(const GetRealPathReq &req);

  OpPtr<OpenRsp> open(OpenReq &req);

  OpPtr<CreateRsp> tryOpen(CreateReq &req);

  OpPtr<MkdirsRsp> mkdirs(const MkdirsReq &req);

  OpPtr<SymlinkRsp> symlink(const SymlinkReq &req);

  OpPtr<RemoveRsp> remove(const RemoveReq &req);

  OpPtr<RenameRsp> rename(const RenameReq &req);

  OpPtr<ListRsp> list(const ListReq &req);

  OpPtr<SyncRsp> sync(const SyncReq &req);

  OpPtr<HardLinkRsp> hardLink(const HardLinkReq &req);

  OpPtr<SetAttrRsp> setAttr(const SetAttrReq &req);

  OpPtr<PruneSessionRsp> pruneSession(const PruneSessionReq &req);

  OpPtr<TestRpcRsp> testRpc(const TestRpcReq &req);

  OpPtr<LockDirectoryRsp> lockDirectory(const LockDirectoryReq &req);

 private:
  template <typename>
  FRIEND_TEST(TestRemove, GC);

  template <typename Rsp>
  friend class Operation;

  const Config &config_;
  analytics::StructuredTraceLog<MetaEventTrace> &metaEventTraceLog_;
  std::shared_ptr<Distributor> distributor_;
  std::shared_ptr<InodeIdAllocator> inodeAlloc_;
  std::shared_ptr<ChainAllocator> chainAlloc_;
  std::shared_ptr<FileHelper> fileHelper_;
  std::shared_ptr<SessionManager> sessionManager_;
  std::shared_ptr<GcManager> gcManager_;
  AclCache aclCache_;
};

}  // namespace hf3fs::meta::server
