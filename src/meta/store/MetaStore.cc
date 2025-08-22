#include "meta/store/MetaStore.h"

#include <cassert>
#include <fmt/core.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <utility>

#include "common/app/NodeId.h"
#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Schema.h"
#include "fbs/mgmtd/ChainRef.h"
#include "meta/components/ChainAllocator.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/Operation.h"
#include "meta/store/Utils.h"

namespace hf3fs::meta::server {

class InitFsOp : public IOperation<Void> {
 public:
  InitFsOp(ChainAllocator &chainAlloc, Layout rootLayout)
      : chainAlloc_(chainAlloc),
        rootLayout_(std::move(rootLayout)) {}

  bool isReadOnly() final { return false; }
  CoTryTask<Void> run(IReadWriteTransaction &txn) final {
    XLOGF(INFO, "MetaStore::initFs");
    auto valid = co_await chainAlloc_.checkLayoutValid(rootLayout_);
    if (valid.hasError()) {
      XLOGF(ERR, "RootLayout is not valid, {}", valid.error());
      co_return makeError(std::move(valid.error()));
    }

    // check tree roots exist
    auto exists = [](auto &val) { return val.has_value(); };
    auto root = (co_await Inode::load(txn, InodeId::root())).then(exists);
    auto gcRoot = (co_await Inode::load(txn, InodeId::gcRoot())).then(exists);
    CO_RETURN_ON_ERROR(root);
    CO_RETURN_ON_ERROR(gcRoot);

    if (!*root) {
      // no root, need create a root
      // root Inode's parent is itself, this simplify path resolution: eg /../../../a -> /a
      Inode root = Inode::newDirectory(InodeId::root(),
                                       InodeId::root(),
                                       "/",
                                       Acl::root(),
                                       rootLayout_,
                                       UtcClock::now().castGranularity(1_ms));
      CO_RETURN_ON_ERROR(co_await root.store(txn));
    }
    if (!*gcRoot) {
      // no GC root, need create a GC root
      Inode gcRoot = Inode::newDirectory(InodeId::gcRoot(),
                                         InodeId::gcRoot(),
                                         "/",
                                         Acl::gcRoot(),
                                         Layout() /* Invalid layout */,
                                         UtcClock::now().castGranularity(1_ms));
      CO_RETURN_ON_ERROR(co_await gcRoot.store(txn));
    }

    co_return Void();
  }

  // NOTE: these function won't be called in InitCluster.cc
  void retry(const Status &) final {}
  void finish(const Result<Void> &) final {}

 private:
  ChainAllocator &chainAlloc_;
  Layout rootLayout_;
};

MetaStore::OpPtr<Void> MetaStore::initFileSystem(ChainAllocator &chainAlloc, Layout rootLayout) {
  return std::make_unique<InitFsOp>(chainAlloc, rootLayout);
}

class BenchRpcOp : public ReadOnlyOperation<TestRpcRsp> {
 public:
  BenchRpcOp(MetaStore &store)
      : ReadOnlyOperation<TestRpcRsp>::ReadOnlyOperation<TestRpcRsp>(store) {}
  CoTryTask<TestRpcRsp> run(IReadOnlyTransaction &) override { co_return TestRpcRsp{}; }
};

MetaStore::OpPtr<TestRpcRsp> MetaStore::testRpc(const TestRpcReq &) { return std::make_unique<BenchRpcOp>(*this); }

}  // namespace hf3fs::meta::server
