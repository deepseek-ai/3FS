#include <algorithm>
#include <fcntl.h>
#include <folly/Likely.h>
#include <folly/Unit.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>
#include <type_traits>

#include "common/monitor/Recorder.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Service.h"
#include "fbs/meta/Utils.h"
#include "meta/store/BatchContext.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"

namespace hf3fs::meta::server {

namespace {
monitor::CountRecorder statFile("meta_server.stat_file");
monitor::CountRecorder statDir("meta_server.stat_dir");
monitor::CountRecorder statSymlink("meta_server.stat_symlink");
}  // namespace

/** MetaStore::stat */
class StatOp : public ReadOnlyOperation<StatRsp> {
 public:
  StatOp(MetaStore &meta, const StatReq &req)
      : ReadOnlyOperation<StatRsp>(meta),
        req_(req) {}

  OPERATION_TAGS(req_);

  CoTryTask<StatRsp> run(IReadOnlyTransaction &txn) override {
    XLOGF(DBG, "StatOp::run, req {}", req_);

    CHECK_REQUEST(req_);

    auto stat = co_await resolve(txn, req_.user)
                    .inode(req_.path, req_.flags, !config().allow_stat_deleted_inodes() /* checkRefCnt */);
    CO_RETURN_ON_ERROR(stat);

    switch (stat->getType()) {
      case InodeType::File:
        statFile.addSample(1);
        break;
      case InodeType::Directory:
        statDir.addSample(1);
        break;
      case InodeType::Symlink:
        statSymlink.addSample(1);
        break;
    }

    co_return StatRsp(std::move(*stat));
  }

 private:
  const StatReq &req_;
};

template <typename Req, typename Rsp>
class BatchStatOp : public ReadOnlyOperation<Rsp> {
 public:
  BatchStatOp(MetaStore &meta, const Req &req)
      : ReadOnlyOperation<Rsp>(meta),
        req_(req) {}

  OPERATION_TAGS(req_);

  auto &vector() {
    if constexpr (std::is_same_v<Req, BatchStatReq>) {
      return req_.inodeIds;
    } else {
      return req_.paths;
    }
  }

  auto createBatchContext() {
    if constexpr (std::is_same_v<Req, BatchStatReq>) {
      return folly::Unit{};
    } else {
      return BatchContext::create();
    }
  }

  CoTryTask<Inode> resolve(IReadOnlyTransaction &txn, const PathAt &path) {
    co_return co_await ReadOnlyOperation<Rsp>::resolve(txn, req_.user)
        .inode(path, req_.flags, !this->config().allow_stat_deleted_inodes() /* checkRefCnt */);
  }

  CoTryTask<Rsp> run(IReadOnlyTransaction &txn) override {
    XLOGF(DBG, "BatchStatOp::run, req {}", req_);
    CHECK_REQUEST(req_);

    static constexpr auto byPath = !std::is_same_v<Req, BatchStatReq>;
    using ResultType = std::conditional_t<byPath, Result<meta::Inode>, std::optional<meta::Inode>>;
    using TaskResultType = std::conditional_t<byPath, Result<Inode>, Result<std::optional<Inode>>>;

    size_t concurrent =
        std::max(1u, !byPath ? this->config().batch_stat_concurrent() : this->config().batch_stat_by_path_concurrent());
    auto exec = co_await folly::coro::co_current_executor;
    [[maybe_unused]] auto guard = createBatchContext();

    std::vector<ResultType> inodes;
    auto iter = vector().begin();
    while (iter != vector().end()) {
      std::vector<folly::SemiFuture<TaskResultType>> tasks;
      while (iter != vector().end() && tasks.size() < concurrent) {
        if constexpr (!byPath) {
          tasks.push_back(Inode::snapshotLoad(txn, *iter).scheduleOn(exec).start());
        } else {
          static_assert(std::is_same_v<Req, BatchStatByPathReq>);
          tasks.push_back(resolve(txn, *iter).scheduleOn(exec).start());
        }
        iter++;
      }
      auto results = co_await folly::coro::collectAllRange(std::move(tasks));
      for (auto result : results) {
        if (result.hasError() && (!byPath || !ErrorHandling::success(result))) {
          XLOGF(INFO, "batch stat error {}", result.error());
          CO_RETURN_ERROR(result);
        }
        if constexpr (byPath) {
          inodes.push_back(result);
        } else {
          inodes.push_back(*result);
        }
      }
    }

    co_return Rsp(std::move(inodes));
  }

 private:
  const Req &req_;
};

MetaStore::OpPtr<StatRsp> MetaStore::stat(const StatReq &req) { return std::make_unique<StatOp>(*this, req); }

MetaStore::OpPtr<BatchStatRsp> MetaStore::batchStat(const BatchStatReq &req) {
  return std::make_unique<BatchStatOp<BatchStatReq, BatchStatRsp>>(*this, req);
}

MetaStore::OpPtr<BatchStatByPathRsp> MetaStore::batchStatByPath(const BatchStatByPathReq &req) {
  return std::make_unique<BatchStatOp<BatchStatByPathReq, BatchStatByPathRsp>>(*this, req);
}

}  // namespace hf3fs::meta::server
