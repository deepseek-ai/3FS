#pragma once

#include <cassert>
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "common/kv/ITransaction.h"
#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Service.h"
#include "fbs/meta/Utils.h"
#include "fdb/FDBRetryStrategy.h"
#include "fdb/FDBTransaction.h"
#include "meta/components/AclCache.h"
#include "meta/components/GcManager.h"
#include "meta/components/SessionManager.h"
#include "meta/event/Event.h"
#include "meta/store/Idempotent.h"
#include "meta/store/MetaStore.h"

#define OPERATION_TAGS(reqName)                                                       \
  std::string_view name() const override { return MetaSerde<>::getRpcName(reqName); } \
  flat::Uid user() const override { return reqName.user.uid; }

#define CHECK_REQUEST(reqName)                                              \
  do {                                                                      \
    if (auto result = reqName.valid(); UNLIKELY(result.hasError())) {       \
      auto rpcName = MetaSerde<>::getRpcName(reqName);                      \
      XLOGF(WARN, "{} get invalid req, error {}", rpcName, result.error()); \
      CO_RETURN_ERROR(result);                                              \
    }                                                                       \
  } while (0)

namespace hf3fs::meta::server {

template <typename Rsp>
class Operation : public IOperation<Rsp> {
 public:
  Operation(MetaStore &meta)
      : meta_(meta) {}

  bool isReadOnly() override { return false; }
  CoTryTask<Rsp> run(IReadWriteTransaction &) override = 0;

  void retry(const Status &) override { clearEvents(); }

  void finish(const Result<Rsp> &result) override {
    if (!result.hasError()) {
      // success
      for (const auto &event : events_) {
        event.log();
      }

      for (const auto &trace : traces_) {
        auto &traceLog = meta_.getEventTraceLog();
        traceLog.append(trace);
      }
    }
  }

 protected:
  const Config &config() const { return meta_.config_; }

  InodeIdAllocator &inodeIdAlloc() { return *meta_.inodeAlloc_; }
  ChainAllocator &chainAlloc() { return *meta_.chainAlloc_; }
  FileHelper &fileHelper() { return *meta_.fileHelper_; }
  SessionManager &sessionManager() { return *meta_.sessionManager_; }
  GcManager &gcManager() { return *meta_.gcManager_; }
  AclCache &aclCache() { return meta_.aclCache_; }
  Distributor &distributor() { return *meta_.distributor_; }

  UtcTime now() const { return UtcClock::now().castGranularity(config().time_granularity()); }

  PathResolveOp resolve(IReadOnlyTransaction &txn, const UserInfo &user, Path *path = nullptr) {
    return PathResolveOp(txn,
                         aclCache(),
                         user,
                         path,
                         config().max_symlink_count(),
                         config().max_symlink_depth(),
                         config().acl_cache_time());
  }

  CoTryTask<InodeId> allocateInodeId(IReadWriteTransaction &txn, bool newChunkEngine) {
    auto newId = co_await inodeIdAlloc().allocate();
    CO_RETURN_ON_ERROR(newId);

    if (newChunkEngine) {
      newId = InodeId::withNewChunkEngine(*newId);
    }

    if (config().inodeId_check_unique()) {
      auto loadResult = co_await Inode::load(txn, *newId);
      CO_RETURN_ON_ERROR(loadResult);
      if (loadResult->has_value()) {
        XLOGF_IF(FATAL,
                 config().inodeId_abort_on_duplicate(),
                 "InodeIdAllocator get duplicated InodeId {}",
                 newId.value());
        XLOGF(DFATAL, "InodeIdAllocator get duplicated InodeId {}", newId.value());
        co_return makeError(MetaCode::kInodeIdAllocFailed);
      }
    } else {
      XLOGF_EVERY_MS(WARN, (300 * 1000), "inodeId_check_unique is disabled");
    }

    co_return newId;
  }

  void clearEvents() { events_.clear(); }
  Event &addEvent(Event::Type type) {
    events_.emplace_back(type);
    return *events_.rbegin();
  }
  void addEvent(Event event) { events_.emplace_back(std::move(event)); }

  void addTrace(MetaEventTrace &&trace) { traces_.emplace_back(std::move(trace)); }

  MetaStore &meta_;
  std::vector<Event> events_;
  std::vector<MetaEventTrace> traces_;
};

template <typename Rsp>
class ReadOnlyOperation : public Operation<Rsp> {
 public:
  ReadOnlyOperation(MetaStore &meta)
      : Operation<Rsp>(meta) {}

  bool isReadOnly() final { return true; }

  virtual CoTryTask<Rsp> run(IReadOnlyTransaction &) = 0;
  CoTryTask<Rsp> run(IReadWriteTransaction &txn) final {
    co_return co_await run(static_cast<IReadOnlyTransaction &>(txn));
  }
};

template <typename Rsp, typename ReqInfo>
class OperationDriver {
 public:
  OperationDriver(MetaStore::Op<Rsp> &operation, const ReqInfo &req, std::optional<SteadyTime> deadline = std::nullopt)
      : operation_(operation),
        req_(req),
        deadline_(deadline) {}

  CoTryTask<Rsp> run(std::unique_ptr<kv::IReadWriteTransaction> txn,
                     kv::FDBRetryStrategy::Config config,
                     bool readonly,
                     bool enableGrvCache) {
    config.retryMaybeCommitted = operation_.retryMaybeCommitted();
    kv::FDBRetryStrategy strategy(config);
    CO_RETURN_ON_ERROR(strategy.init(txn.get()));

    OperationRecorder::Guard recorder(OperationRecorder::server(), operation_.name(), operation_.user());

    if (readonly && !operation_.isReadOnly()) {
      co_return makeError(StatusCode::kReadOnlyMode, "FileSystem is in readonly mode.");
    }

    auto grvCache = operation_.isReadOnly() && enableGrvCache;
    if (grvCache && dynamic_cast<kv::FDBTransaction *>(txn.get())) {
      auto fdbTxn = dynamic_cast<kv::FDBTransaction *>(txn.get());
      CO_RETURN_ON_ERROR(fdbTxn->setOption(FDBTransactionOption::FDB_TR_OPTION_USE_GRV_CACHE, {}));
    }

    Result<Rsp> result = makeError(MetaCode::kOperationTimeout);
    auto duplicate = false;
    while (true) {
      // check timeout
      if (deadline_ && deadline_.value() <= SteadyClock::now()) {
        XLOGF(ERR, "Request {} timeout, return error {}", describe(), result);
        break;
      }
      // run operation
      result = co_await runAndCommit(*txn, operation_, duplicate);
      if (ErrorHandling::success(result)) {
        break;
      }
      // retry
      XLOGF(WARN, "Request {} failed, error {}", describe(), result.error());
      operation_.retry(result.error());
      auto retry = co_await strategy.onError(txn.get(), result.error());
      if (retry.hasError()) {
        result = makeError(retry.error());
        break;
      }
      recorder.retry()++;
    }

    if (result.hasError() && result.error().code() == StatusCode::kOK) {
      XLOGF(DFATAL, "Has error but error code is kOK, {}, {}", describe(), result);
      result = makeError(MetaCode::kFoundBug);
    }

    recorder.finish(result, duplicate);
    operation_.finish(result);
    co_return result;
  }

 private:
#define IDEMPOTENT_CHECK()                                                           \
  do {                                                                               \
    auto idemCheck = co_await Idempotent::load<Rsp>(txn, clientId, requestId, req_); \
    CO_RETURN_ON_ERROR(idemCheck);                                                   \
    duplicate = idemCheck->has_value();                                              \
    if (duplicate) {                                                                 \
      co_return idemCheck->value();                                                  \
    }                                                                                \
  } while (0)

  template <typename Handler>
  std::invoke_result_t<Handler, IReadWriteTransaction &> runAndCommit(IReadWriteTransaction &txn,
                                                                      Handler &&handler,
                                                                      bool &duplicate) {
    Uuid clientId, requestId;
    auto readonly = handler.isReadOnly();
    auto idem = !readonly && operation_.needIdempotent(clientId, requestId);
    if (idem) {
      OperationRecorder::server().addIdempotentCount();
      IDEMPOTENT_CHECK();
      auto result = co_await handler(txn);
      if (result) {
        CO_RETURN_ON_ERROR(co_await Idempotent::store(txn, clientId, requestId, result));
        CO_RETURN_ON_ERROR(co_await txn.commit());
      } else if (ErrorHandling::success(result) || !ErrorHandling::retryable(result.error())) {
        // this is final result, discard other modifications and save result
        txn.reset();
        IDEMPOTENT_CHECK();
        CO_RETURN_ON_ERROR(co_await Idempotent::store(txn, clientId, requestId, result));
        CO_RETURN_ON_ERROR(co_await txn.commit());
      }
      co_return result;
    } else {
      auto result = co_await handler(txn);
      if (!result.hasError() && !readonly) {
        CO_RETURN_ON_ERROR(co_await txn.commit());
      }
      co_return result;
    }
  }

  std::string describe() const {
    if constexpr (std::is_base_of_v<ReqBase, ReqInfo>) {
      return fmt::format("{}{}", operation_.name(), req_);
    } else {
      return std::string(operation_.name());
    }
  }

  MetaStore::Op<Rsp> &operation_;
  const ReqInfo &req_;
  std::optional<SteadyTime> deadline_;
};

}  // namespace hf3fs::meta::server