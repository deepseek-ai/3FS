#pragma once

#include <atomic>
#include <cassert>
#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/experimental/coro/Baton.h>
#include <folly/logging/xlog.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/Shards.h"
#include "common/utils/UtcTime.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"
namespace hf3fs::meta::server {

class MetaOperator;

class BatchedOp : public Operation<Inode> {
 public:
  template <typename Req, typename Rsp>
  struct Waiter : folly::NonCopyableNonMovable {
    Req req;
    std::optional<Result<Rsp>> result;
    folly::coro::Baton baton;
    bool newFile = false; /* for Create operation */

    Waiter(Req req)
        : req(std::move(req)) {}

    Result<Rsp> getResult() {
      XLOGF_IF(FATAL, !result.has_value(), "result not set");
      return *result;
    }

    bool hasError() const { return result.has_value() && result->hasError(); }

    void finish(BatchedOp &op, const Result<Inode> &r);
  };

  BatchedOp(MetaStore &meta, InodeId inodeId)
      : Operation(meta),
        inodeId_(inodeId) {}

  std::string_view name() const override { return "batchedOp"; }

  flat::Uid user() const override { return user_; }

  template <typename Req, typename Rsp>
  void add(Waiter<Req, Rsp> &waiter);

  template <>
  void add(Waiter<SyncReq, SyncRsp> &waiter) {
    XLOGF_IF(FATAL, waiter.req.inode != inodeId_, "{} != {}", waiter.req.inode, inodeId_);
    addReq(syncs_, waiter);
  }

  template <>
  void add(Waiter<CloseReq, CloseRsp> &waiter) {
    XLOGF_IF(FATAL, waiter.req.inode != inodeId_, "{} != {}", waiter.req.inode, inodeId_);
    addReq(closes_, waiter);
  }

  template <>
  void add(Waiter<SetAttrReq, SetAttrRsp> &waiter) {
    XLOGF_IF(FATAL, waiter.req.path != PathAt(inodeId_), "{} != {}", waiter.req.path, PathAt(inodeId_));
    addReq(setattrs_, waiter);
  }

  template <>
  void add(Waiter<CreateReq, CreateRsp> &waiter) {
    XLOGF_IF(FATAL,
             (waiter.req.path.parent != inodeId_ || !waiter.req.path.path || waiter.req.path.path->has_parent_path()),
             "path {}, inodeId {}",
             waiter.req.path,
             inodeId_);
    addReq(creates_, waiter);
  }

  CoTryTask<Inode> run(IReadWriteTransaction &txn) override;

  void retry(const Status &error) override;

  void finish(const Result<Inode> &result) override;

  size_t numReqs() const { return numReqs_; }

  // for test
  static CoTryTask<CreateRsp> create(MetaStore &store, IReadWriteTransaction &txn, CreateReq req) {
    Waiter<CreateReq, CreateRsp> waiter(req);
    BatchedOp op(store, req.path.parent);
    op.add(waiter);
    op.finish(co_await op.run(txn));
    co_return waiter.getResult();
  }

 private:
  friend class MetaOperator;

  template <typename Req, typename Rsp>
  using WaiterRef = std::reference_wrapper<Waiter<Req, Rsp>>;

  void addReq(auto &reqs, auto &waiter) {
    if (!user_) {
      user_ = waiter.req.user.uid;
    }
    reqs.emplace_back(waiter);
    numReqs_++;
  }

  CoTryTask<bool> setAttr(IReadWriteTransaction &txn, Inode &inode);

  CoTryTask<bool> syncAndClose(IReadWriteTransaction &txn, Inode &inode);

  CoTryTask<bool> sync(Inode &inode,
                       const SyncReq &req,
                       bool &updateLength,
                       bool &truncate,
                       std::optional<VersionedLength> &hintLength);

  CoTryTask<bool> close(Inode &inode,
                        const CloseReq &req,
                        bool &updateLength,
                        std::optional<VersionedLength> &hintLength,
                        std::vector<FileSession> &sessions);

  CoTryTask<bool> create(IReadWriteTransaction &txn, Inode &inode);

  CoTryTask<bool> create(IReadWriteTransaction &txn,
                         const Inode &parent,
                         folly::Synchronized<uint32_t> &chainAllocCounter,
                         auto begin,
                         auto end);

  CoTryTask<std::pair<Inode, DirEntry>> create(IReadWriteTransaction &txn,
                                               const Inode &parent,
                                               folly::Synchronized<uint32_t> &chainAllocCounter,
                                               const CreateReq &req);

  CoTryTask<void> openExists(IReadWriteTransaction &txn, Inode &inode, const DirEntry &entry, auto begin, auto end);

  CoTryTask<bool> openExists(IReadWriteTransaction &txn, Inode &inode, const CreateReq &req);

  CoTryTask<VersionedLength> queryLength(const Inode &inode, std::optional<VersionedLength> hintLength, bool truncate);

  // requests
  InodeId inodeId_;
  flat::Uid user_;  // use first uid
  std::vector<WaiterRef<SetAttrReq, SetAttrRsp>> setattrs_;
  std::vector<WaiterRef<SyncReq, SyncRsp>> syncs_;
  std::vector<WaiterRef<CloseReq, CloseRsp>> closes_;
  std::vector<WaiterRef<CreateReq, CreateRsp>> creates_;
  size_t numReqs_ = 0;

  // state
  std::optional<kv::Versionstamp> versionstamp_;
  std::optional<VersionedLength> currLength_;
  std::optional<VersionedLength> nextLength_;
};

}  // namespace hf3fs::meta::server