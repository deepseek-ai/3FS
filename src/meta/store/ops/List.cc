#include <fcntl.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <utility>

#include "common/kv/ITransaction.h"
#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"
#include "meta/store/Utils.h"

namespace hf3fs::meta::server {

namespace {
monitor::DistributionRecorder listDist("meta_server:list_entries");
}

/** MetaStore::list */
class ListOp : public ReadOnlyOperation<ListRsp> {
 public:
  ListOp(MetaStore &meta, const ListReq &req)
      : ReadOnlyOperation<ListRsp>(meta),
        req_(req) {}

  OPERATION_TAGS(req_);

  CoTryTask<ListRsp> run(IReadOnlyTransaction &txn) override {
    XLOGF(DBG, "ListOp: {}", req_);

    CHECK_REQUEST(req_);

    auto result =
        co_await resolve(txn, req_.user).inode(req_.path, AtFlags(AtFlags(AT_SYMLINK_FOLLOW)), true /* checkRefCnt */);
    CO_RETURN_ON_ERROR(result);

    auto &inode = result.value();
    if (!inode.isDirectory()) {
      co_return makeError(MetaCode::kNotDirectory);
    }
    if (!inode.acl.checkPermission(req_.user, AccessType::READ)) {
      co_return makeError(MetaCode::kNoPermission);
    }

    auto list = co_await DirEntryList::snapshotLoad(txn,
                                                    inode.id,
                                                    req_.prev,
                                                    req_.limit > 0 ? req_.limit : config().list_default_limit(),
                                                    req_.status,
                                                    config().batch_stat_concurrent());
    CO_RETURN_ON_ERROR(list);

    listDist.addSample(list->entries.size(), {{"uid", folly::to<std::string>(req_.user.uid)}});

    co_return ListRsp(std::move(*list));
  }

 private:
  const ListReq &req_;
};

MetaStore::OpPtr<ListRsp> MetaStore::list(const ListReq &req) { return std::make_unique<ListOp>(*this, req); }

}  // namespace hf3fs::meta::server
