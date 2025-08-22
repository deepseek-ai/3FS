#include "MetaClient.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <fcntl.h>
#include <fmt/format.h>
#include <folly/Math.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/functional/Partial.h>
#include <folly/futures/Future.h>
#include <folly/io/async/Request.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ServerSelectionStrategy.h"
#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"
#include "common/serde/MessagePacket.h"
#include "common/serde/Service.h"
#include "common/utils/Address.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/RequestInfo.h"
#include "common/utils/Result.h"
#include "common/utils/SemaphoreGuard.h"
#include "common/utils/Status.h"
#include "common/utils/StatusCode.h"
#include "common/utils/SysResource.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/FileOperation.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "fbs/meta/Utils.h"
#include "fbs/mgmtd/RoutingInfo.h"
#include "fmt/core.h"
#include "stubs/MetaService/IMetaServiceStub.h"

#define OPTIONAL_TIME(opt) ((opt) ? std::optional(UtcClock::now()) : std::nullopt)
#define OPTIONAL_SESSION(sessionId) \
  ((sessionId).has_value() ? std::optional(meta::SessionInfo(clientId_, *sessionId)) : std::nullopt)

#define RETURN_VOID [](auto &) { return Void{}; }
#define EXTRACT(name) [](auto &rsp) { return std::move(rsp.name); }

#define CHECK_REQUEST(varName)                                          \
  do {                                                                  \
    if (auto result = varName.valid(); UNLIKELY(result.hasError())) {   \
      auto rpcName = MetaSerde<>::getRpcName(varName);                  \
      XLOGF(WARN, "{} invalid req, error {}", rpcName, result.error()); \
      CO_RETURN_ERROR(result);                                          \
    }                                                                   \
  } while (0)

#define RECORD_LATENCY(latency)                                                                                       \
  auto FB_ANONYMOUS_VARIABLE(guard) = folly::makeGuard([lat = &(latency), begin = std::chrono::steady_clock::now()] { \
    lat->addSample(std::chrono::steady_clock::now() - begin);                                                         \
  })

#define RECORD_TIMESATAMP(name, result, timestamp)                                             \
  do {                                                                                         \
    auto &&r = result;                                                                         \
    if (r || hf3fs::StatusCode::typeOf(r.error().code()) != hf3fs::StatusCodeType::RPC) {      \
      inflightTime.addSample(timestamp.inflightLatency(), {{"instance", std::string(name)}});  \
      serverLatency.addSample(timestamp.serverLatency(), {{"instance", std::string(name)}});   \
      networkLatency.addSample(timestamp.networkLatency(), {{"instance", std::string(name)}}); \
    }                                                                                          \
  } while (0)

#define ASSERT_NO_REQUEST_CONTEXT()                                            \
  do {                                                                         \
    auto ctx = folly::RequestContext::try_get();                               \
    XLOGF_IF(FATAL, ctx != nullptr, "RequestContext {} != NULL", (void *)ctx); \
  } while (0)

namespace hf3fs::meta::client {

using namespace stubs;

namespace {
monitor::CountRecorder serverError("meta_client.server_error");
monitor::CountRecorder reqRejected("meta_client.req_rejected");
monitor::LatencyRecorder getServerLatency("meta_client.get_server");
monitor::CountRecorder truncateTooManyChunks("meta_client.truncate_too_many_chunks");
monitor::DistributionRecorder truncateIters("meta_client.truncate_iters");

monitor::LatencyRecorder inflightTime{"meta_client.inflight_time"};
monitor::LatencyRecorder serverLatency{"meta_client.server_latency"};
monitor::LatencyRecorder networkLatency{"meta_client.network_latency"};

FileOperation::Recorder recorder("meta_client");
}  // namespace

MetaClient::MetaClient(ClientId clientId,
                       const Config &config,
                       std::unique_ptr<StubFactory> factory,
                       std::shared_ptr<ICommonMgmtdClient> mgmtd,
                       std::shared_ptr<storage::client::StorageClient> storage,
                       bool dynStripe)
    : clientId_(clientId),
      config_(config),
      dynStripe_(dynStripe),
      factory_(std::move(factory)),
      mgmtd_(std::move(mgmtd)),
      storage_(std::move(storage)),
      serverSelection_(),
      onConfigUpdated_(),
      concurrentReqSemaphore_(config_.max_concurrent_requests()) {
  auto podName = SysResource::hostname(false);
  auto hostName = SysResource::hostname(true);
  XLOGF_IF(ERR, podName.hasError(), "Failed to get pod name, error {}", podName.error());
  XLOGF_IF(ERR, hostName.hasError(), "Failed to get host name, error {}", hostName.error());
  if (podName.hasValue() && hostName.hasValue()) {
    if (*podName != *hostName) {
      clientId_.hostname = fmt::format("{}@{}", *podName, *hostName);
    } else {
      clientId_.hostname = *podName;
    }
  }

  assert(mgmtd_);
  assert(factory_);
  serverSelection_ = ServerSelectionStrategy::create(config.selection_mode(), mgmtd_, config.network_type());
  onConfigUpdated_ = config_.addCallbackGuard([this]() {
    auto strategy = serverSelection_.load();
    if (strategy->mode() != config_.selection_mode() || strategy->addrType() != config_.network_type()) {
      XLOGF(INFO,
            "MetaClient turn to server selection mode {}, network {}",
            magic_enum::enum_name(config_.selection_mode()),
            magic_enum::enum_name(config_.network_type()));
      serverSelection_ = ServerSelectionStrategy::create(config_.selection_mode(), mgmtd_, config_.network_type());
    }
    if (config_.max_concurrent_requests() != concurrentReqSemaphore_.getUsableTokens()) {
      concurrentReqSemaphore_.changeUsableTokens(config_.max_concurrent_requests());
    }
  });
}

void MetaClient::start(CPUExecutorGroup &exec) {
  bgCloser_ = std::make_unique<CoroutinesPool<CloseTask>>(config_.background_closer().coroutine_pool());
  bgCloser_->start(folly::partial(&MetaClient::runCloseTask, this), exec);

  bgRunner_ = std::make_unique<BackgroundRunner>(&exec.pickNext());
  bgRunner_->start("ScanCloseTask",
                   folly::partial(&MetaClient::scanCloseTask, this),
                   config_.background_closer().task_scan_getter());
  bgRunner_->start("CheckServer",
                   folly::partial(&MetaClient::checkServers, this),
                   config_.check_server_interval_getter());
  XLOGF(INFO, "MetaClient started");
}

void MetaClient::stop() {
  if (bgRunner_) {
    folly::coro::blockingWait(bgRunner_->stopAll());
    bgRunner_.reset();
  }
  if (bgCloser_) {
    bgCloser_->stopAndJoin();
    bgCloser_.reset();
  }

  XLOGF(INFO, "MetaClient stopped");
}

void MetaClient::enqueueCloseTask(CloseTask task) {
  XLOGF(INFO, "MetaClient enqueue close task {}.", task.describe());
  auto now = SteadyClock::now();
  auto deadline = now + task.backoff;
  bgCloseTasks_.lock()->emplace(deadline, std::move(task));
}

CoTask<void> MetaClient::scanCloseTask() {
  ASSERT_NO_REQUEST_CONTEXT();
  XLOGF(DBG, "MetaClient scan background close tasks, count {}.", bgCloseTasks_.lock()->size());
  auto now = SteadyClock::now();

  // handle prune session batches
  auto sessions = batchPrune_.lock()->take(config_.background_closer().prune_session_batch_count(),
                                           config_.background_closer().prune_session_batch_interval());
  if (!sessions.empty()) {
    CloseTask task{PruneSessionReq(clientId_, std::move(sessions)), config_.background_closer().retry_first_wait()};
    co_await bgCloser_->enqueue(std::move(task));
  }

  // handle retry tasks
  while (true) {
    auto guard = bgCloseTasks_.lock();
    if (guard->empty()) {
      break;
    }
    auto iter = guard->begin();
    auto deadline = iter->first;
    if (deadline > now) {
      break;
    }
    XLOGF(INFO, "MetaClient find background close task");
    auto task = std::move(iter->second);
    guard->erase(iter);
    guard.unlock();

    co_await bgCloser_->enqueue(std::move(task));
  }
}

CoTask<void> MetaClient::runCloseTask(CloseTask task) {
  ASSERT_NO_REQUEST_CONTEXT();
  XLOGF(INFO, "MetaClient run close task {}", task.describe());

  bool needRetry = false;
  Result<Void> result = Void{};
  XLOGF_IF(FATAL, task.req.valueless_by_exception(), "Req is valueless");
  if (std::holds_alternative<CloseReq>(task.req)) {
    result = (co_await tryClose(std::get<CloseReq>(task.req), needRetry)).then(RETURN_VOID);
  } else if (std::holds_alternative<PruneSessionReq>(task.req)) {
    result = (co_await tryPrune(std::get<PruneSessionReq>(task.req), needRetry)).then(RETURN_VOID);
  }

  if (result.hasError()) {
    XLOGF(ERR, "MetaClient failed to run close task {}, retry {}", task.describe(), needRetry);
    if (needRetry) {
      auto max = config_.background_closer().retry_max_wait();
      auto next = Duration(task.backoff * 2);
      task.backoff = std::min(next, max);
      enqueueCloseTask(std::move(task));
    }
  } else {
    assert(!needRetry);
    XLOGF(DBG, "MetaClient finished close task {}", task.describe());
  }

  co_return;
}

CoTryTask<MetaClient::ServerNode> MetaClient::getServerNode() {
  RECORD_LATENCY(getServerLatency);
  if (!mgmtd_) {
    co_return MetaClient::ServerNode({flat::NodeId(0), net::Address(), "mock"}, factory_->create(net::Address()));
  } else {
    auto node = serverSelection_.load()->select(*errNodes_.rlock());
    CO_RETURN_ON_ERROR(node);
    auto stub = factory_->create(node->address, node->nodeId, node->hostname);
    co_return MetaClient::ServerNode(std::move(*node), std::move(stub));
  }
}

CoTask<void> MetaClient::checkServers() {
  ASSERT_NO_REQUEST_CONTEXT();

  auto check = [&](ServerSelectionStrategy::NodeInfo node) -> CoTask<void> {
    auto stub = factory_->create(node.address, node.nodeId, node.hostname);
    auto options = net::UserRequestOptions();
    options.timeout = config_.retry_default().rpc_timeout();
    options.sendRetryTimes = config_.retry_default().retry_send();
    auto timestamp = serde::Timestamp();
    TestRpcReq req;
    auto result = co_await (stub)->testRpc(req, options, &timestamp);
    RECORD_TIMESATAMP("testRpc", result, timestamp);
    if (result.hasError()) {
      XLOGF(WARN, "MetaClient check node {} failed, error {}", node.nodeId, result);
    } else {
      XLOGF(INFO, "MetaClient check node {} success.", node.nodeId);
      errNodes_.wlock()->erase(node.nodeId);
    }
  };

  std::vector<folly::SemiFuture<Void>> tasks;
  auto guard = errNodes_.wlock();
  auto iter = guard->begin();
  while (iter != guard->end()) {
    auto nodeId = *iter;
    auto node = serverSelection_.load()->get(nodeId);
    if (!node.has_value()) {
      XLOGF(INFO, "Node {} not found", nodeId);
      iter = guard->erase(iter);
    } else {
      tasks.push_back(
          folly::coro::co_invoke(check, *node).scheduleOn(co_await folly::coro::co_current_executor).start());
      iter++;
    }
  }
  guard.unlock();

  co_await folly::coro::collectAllRange(std::move(tasks));
}

template <typename Req>
static std::string reqDescribe(const Req &req) {
  if constexpr (std::is_same_v<Req, StatReq> || std::is_same_v<Req, OpenReq>) {
    return fmt::format("{{'path':{}, flags:'0x{:x}'}}", req.path, req.flags.toUnderType());
  } else {
    return fmt::format("{}", req);
  }
}

static std::string rspDescribe(const Status &status) {
  switch (status.code()) {
    case MetaCode::kNotFound:
      return fmt::format("{}", Status(MetaCode::kNotFound));
    default:
      return fmt::format("{}", status);
  }
}

template <typename Rsp>
CoTryTask<Void> MetaClient::waitRoutingInfo(const Rsp &rsp, const RetryConfig &retryConfig) {
  auto begin = SteadyClock::now();
  auto waitTime = retryConfig.retry_total_time() / 2;
  while (true) {
    auto routingInfo = mgmtd_->getRoutingInfo()->raw();
    if (RoutingInfoChecker::checkRoutingInfo(rsp, *routingInfo)) {
      co_return Void{};
    }

    if (begin + waitTime < SteadyClock::now()) {
      XLOGF(ERR, "routing info not ready, rsp {}", rsp);
      co_return makeError(MgmtdClientCode::kRoutingInfoNotReady);
    }

    XLOGF(WARN, "wait new routing Info, rsp {}", rsp);
    co_await folly::coro::sleep(std::chrono::seconds(1));
  }
}

template <typename Func, typename Req>
auto MetaClient::retry(Func &&func, Req &&req, RetryConfig retryConfig, std::function<void(const Status &)> onError)
    -> std::invoke_result_t<Func, Stub::IStub, Req &&, const net::UserRequestOptions &, serde::Timestamp *> {
  req.client = clientId_;
  CHECK_REQUEST(req);

  // auto reqInfo = RequestInfo::get();
  auto opName = MetaSerde<>::getRpcName(req);
  ExponentialBackoffRetry backoff(retryConfig.retry_init_wait().asMs(),
                                  retryConfig.retry_max_wait().asMs(),
                                  retryConfig.retry_total_time().asMs());
  auto options = net::UserRequestOptions();
  options.timeout = retryConfig.rpc_timeout();

  OperationRecorder::Guard record(OperationRecorder::client(), opName, req.user.uid);
  std::optional<ServerNode> server;

  auto getServer = [&]() -> CoTryTask<void> {
    if (server.has_value()) {
      co_return Void{};
    }
    auto result = co_await getServerNode();
    if (!result.hasError()) {
      server.emplace(std::move(*result));
      co_return Void{};
    }
    auto waitTime = backoff.getWaitTime();
    XLOGF(ERR,
          "Op {} get server failed, req {}, error {}, retried {}, wait {}",
          opName,
          req,
          result.error(),
          record.retry(),
          waitTime);
    if (waitTime.count() == 0) {
      CO_RETURN_ERROR(result);
    }
    co_await folly::coro::sleep(waitTime);
    co_return Void{};
  };

  while (true) {
    CO_RETURN_ON_ERROR(co_await getServer());
    if (!server.has_value()) {
      continue;
    }

    Status error(MetaCode::kFoundBug);
    {
      SemaphoreGuard concurrentReq(concurrentReqSemaphore_);
      co_await concurrentReq.coWait();

      // if (reqInfo && reqInfo->canceled()) {
      //   XLOGF(WARN, "Op {}{} canceled by client {}", opName, req, reqInfo->describe());
      //   record.finish(Status(MetaCode::kRequestCanceled));
      //   co_return makeError(MetaCode::kRequestCanceled, "req canceled");
      // }

      serde::Timestamp timestamp;
      auto result = co_await (server->stub.get()->*func)(std::forward<Req>(req), options, &timestamp);
      RECORD_TIMESATAMP(opName, result, timestamp);
      if (ErrorHandling::success(result)) {
        // success
        record.finish(result);
        errNodes_.wlock()->erase(server->node.nodeId);
        XLOGF_IF(INFO, result.hasError(), "Op {}{}, result {}", opName, reqDescribe(req), rspDescribe(result.error()));
        XLOGF_IF(DBG, !result.hasError(), "Op {}{}, result {}", opName, req, *result);

        if (result.hasValue()) {
          CO_RETURN_ON_ERROR(co_await waitRoutingInfo(*result, retryConfig));
        }

        co_return result;
      }
      XLOGF_IF(FATAL, !result.hasError(), "Result is ok");
      error = std::move(result.error());
    }
    if (onError) {
      onError(error);
    }

    // retry
    auto waitTime = backoff.getWaitTime();
    switch (error.code()) {
      // retry fast
      case RPCCode::kRequestRefused:
        reqRejected.addSample(1, {{"tag", fmt::format("{}", server->node.nodeId.toUnderType())}});
      case RPCCode::kSendFailed:
      case MetaCode::kBusy:
        waitTime = std::min(retryConfig.retry_init_wait(), retryConfig.retry_fast());
        break;
      // retry default
      default:
        break;
    }
    auto retryable = ErrorHandling::retryable(error);
    auto serverNode = server->node;
    bool failover = false;
    if (ErrorHandling::serverError(error)) {
      serverError.addSample(1, {{"tag", fmt::format("{}", server->node.nodeId.toUnderType())}});
      failover = (++server->failure) >= retryConfig.max_failures_before_failover();
    }
    XLOGF(ERR,
          "Op {} failed on {}, req {}, error {}, retryable {}, failover {}, retried {}, elapsed {}",
          opName,
          server->node,
          req,
          error,
          retryable,
          failover,
          record.retry(),
          backoff.getElapsedTime());
    if (failover) {
      errNodes_.wlock()->insert(server->node.nodeId);
      server = std::nullopt;
    }
    if (!retryable) {
      // can't retry
      record.finish(error);
      co_return makeError(std::move(error));
    }
    if (waitTime.count() == 0 || backoff.getElapsedTime() > retryConfig.retry_total_time().asMs()) {
      XLOGF(CRITICAL, "Op {}{} failed {}, retry timeout", opName, req, error);
      record.finish(error);
      co_return makeError(std::move(error));
    }

    co_await folly::coro::sleep(waitTime);
    record.retry()++;
  }
}

CoTryTask<void> MetaClient::updateLayout(Layout &layout) {
  if (layout.empty() || !std::holds_alternative<Layout::ChainList>(layout.chains) || layout.tableId == 0 ||
      layout.tableVersion != 0) {
    XLOGF(INFO, "Don't need update layout {}", layout);
    co_return Void{};
  }

  auto routing = mgmtd_->getRoutingInfo();
  if (!routing) {
    XLOGF(ERR, "Routing info not ready");
    co_return makeError(MgmtdClientCode::kRoutingInfoNotReady);
  }
  auto table = routing->raw()->getChainTable(layout.tableId);
  if (!table) {
    XLOGF(ERR, "Can't find chain table {}", layout.tableId);
    co_return makeError(MetaCode::kInvalidFileLayout);
  }
  layout.tableVersion = table->chainTableVersion;
  XLOGF(INFO, "Update layout {}", layout);

  if (auto valid = layout.valid(true); valid.hasError()) {
    XLOGF(ERR, "Layout is not valid {}, error {}", layout, valid.error());
    CO_RETURN_ERROR(valid);
  }

  co_return Void{};
}

CoTryTask<UserInfo> MetaClient::authenticate(const UserInfo &userInfo) {
  auto req = AuthReq(userInfo);
  co_return (co_await retry(&IMetaServiceStub::authenticate, req)).then(EXTRACT(user));
}

CoTryTask<Inode> MetaClient::stat(const flat::UserInfo &userInfo,
                                  meta::InodeId inodeId,
                                  const std::optional<Path> &path,
                                  bool followLastSymlink) {
  auto req = StatReq(userInfo,
                     PathAt(inodeId, path),
                     followLastSymlink ? AtFlags(AT_SYMLINK_FOLLOW) : AtFlags(AT_SYMLINK_NOFOLLOW));
  co_return (co_await retry(&IMetaServiceStub::stat, req)).then(EXTRACT(stat));
}

CoTryTask<std::vector<std::optional<Inode>>> MetaClient::batchStat(const UserInfo &userInfo,
                                                                   std::vector<InodeId> inodeIds) {
  auto req = BatchStatReq(userInfo, inodeIds);
  auto rsp = (co_await retry(&IMetaServiceStub::batchStat, req)).then(EXTRACT(inodes));
  if (!rsp.hasError()) {
    XLOGF_IF(DFATAL, rsp->size() != inodeIds.size(), "{} != {}", rsp->size(), inodeIds.size());
    co_return rsp;
  }
  if (rsp.error().code() != RPCCode::kInvalidMethodID) {
    co_return rsp;
  }
  XLOGF(ERR, "batchStat get kInvalidMethodID, need update Meta Server.");
  std::vector<std::optional<Inode>> inodes;
  for (auto &inodeId : inodeIds) {
    auto rsp = co_await stat(userInfo, inodeId, std::nullopt, false);
    if (rsp.hasError() && rsp.error().code() == MetaCode::kNotFound) {
      inodes.push_back(std::nullopt);
      continue;
    }
    CO_RETURN_ON_ERROR(rsp);
    inodes.push_back(std::move(*rsp));
  }
  co_return inodes;
}

CoTryTask<std::vector<Result<Inode>>> MetaClient::batchStatByPath(const UserInfo &userInfo,
                                                                  std::vector<PathAt> paths,
                                                                  bool followLastSymlink) {
  auto req = BatchStatByPathReq(userInfo,
                                paths,
                                followLastSymlink ? AtFlags(AT_SYMLINK_FOLLOW) : AtFlags(AT_SYMLINK_NOFOLLOW));
  auto rsp = (co_await retry(&IMetaServiceStub::batchStatByPath, req)).then(EXTRACT(inodes));
  if (!rsp.hasError()) {
    XLOGF_IF(DFATAL, rsp->size() != paths.size(), "{} != {}", rsp->size(), paths.size());
    co_return rsp;
  }
  if (rsp.error().code() != RPCCode::kInvalidMethodID) {
    co_return rsp;
  }
  XLOGF(ERR, "batchStatByPath get kInvalidMethodID, need update Meta Server.");
  std::vector<Result<Inode>> inodes;
  for (auto &path : paths) {
    auto rsp = co_await stat(userInfo, path.parent, path.path, followLastSymlink);
    if (!ErrorHandling::success(rsp)) {
      CO_RETURN_ERROR(rsp);
    }
    inodes.push_back(std::move(rsp));
  }
  co_return inodes;
}

CoTryTask<StatFsRsp> MetaClient::statFs(const UserInfo &userInfo) {
  auto req = StatFsReq(userInfo);
  co_return co_await retry(&IMetaServiceStub::statFs, req);
}

CoTryTask<Path> MetaClient::getRealPath(const UserInfo &userInfo,
                                        InodeId parent,
                                        const std::optional<Path> &path,
                                        bool absolute) {
  auto req = GetRealPathReq(userInfo, PathAt(parent, path), absolute);
  co_return (co_await retry(&IMetaServiceStub::getRealPath, req)).then(EXTRACT(path));
}

CoTryTask<Inode> MetaClient::openCreate(auto func, auto req) {
  req.dynStripe &= config_.dynamic_stripe();

  bool needPrune = false;
  auto onError = [&](const Status &error) {
    if (ErrorHandling::needPruneSession(error)) {
      needPrune = true;
    }
  };
  auto retryConfig = config_.retry_default().clone();
  auto result = co_await retry(func, req, retryConfig, onError);
  if (result.hasError() && needPrune && req.session.has_value()) {
    co_await pruneSession(req.session->session);
  }
  CO_RETURN_ON_ERROR(result);
  if (result->needTruncate) {
    if (!req.flags.contains(O_TRUNC)) {
      XLOGF(DFATAL, "req {}, O_TRUNC not set", req);
      co_return makeError(MetaCode::kFoundBug, "O_TRUNC not set, but need truncate");
    }
    XLOGF(INFO, "truncate {} because O_TRUNC", result->stat.id);
    auto truncateResult = co_await truncateImpl(req.user, result->stat, 0);
    if (truncateResult.hasError()) {
      co_await close(req.user,
                     result->stat.id,
                     req.session.has_value() ? std::optional(req.session->session) : std::nullopt,
                     false,
                     true);
      CO_RETURN_ERROR(truncateResult);
    }
    co_return std::move(truncateResult);
  }
  co_return std::move(result->stat);
}

CoTryTask<Inode> MetaClient::create(const UserInfo &userInfo,
                                    InodeId parent,
                                    const Path &path,
                                    std::optional<SessionId> sessionId,
                                    Permission perm,
                                    int flags,
                                    std::optional<Layout> layout) {
  if (layout.has_value()) {
    CO_RETURN_ON_ERROR(co_await updateLayout(*layout));
  }
  auto req = CreateReq(userInfo,
                       PathAt(parent, path),
                       OPTIONAL_SESSION(sessionId),
                       flags,
                       meta::Permission(perm & ALLPERMS),
                       layout,
                       dynStripe_ && config_.dynamic_stripe());
  co_return co_await openCreate(&IMetaServiceStub::create, req);
}

CoTryTask<Inode> MetaClient::open(const UserInfo &userInfo,
                                  InodeId inodeId,
                                  const std::optional<Path> &path,
                                  std::optional<SessionId> sessionId,
                                  int flags) {
  if ((flags & O_ACCMODE) == O_RDONLY) sessionId = std::nullopt;
  auto req = OpenReq(userInfo,
                     PathAt(inodeId, path),
                     OPTIONAL_SESSION(sessionId),
                     flags,
                     dynStripe_ && config_.dynamic_stripe());
  co_return co_await openCreate(&IMetaServiceStub::open, req);
}

CoTryTask<Inode> MetaClient::close(const UserInfo &userInfo,
                                   InodeId inodeId,
                                   std::optional<SessionId> sessionId,
                                   bool read,
                                   bool written) {
  co_return co_await close(userInfo, inodeId, sessionId, written, OPTIONAL_TIME(read), OPTIONAL_TIME(written));
}

CoTryTask<Inode> MetaClient::close(const UserInfo &userInfo,
                                   InodeId inodeId,
                                   std::optional<SessionId> sessionId,
                                   bool updateLength,
                                   const std::optional<UtcTime> atime,
                                   const std::optional<UtcTime> mtime) {
  auto req = CloseReq(userInfo, inodeId, OPTIONAL_SESSION(sessionId), updateLength, atime, mtime);

  auto result = (co_await retry(&IMetaServiceStub::close, req)).then(EXTRACT(stat));
  if (result.hasError() && req.session.has_value() && ErrorHandling::retryable(result.error())) {
    CloseTask task{req, config_.background_closer().retry_first_wait()};
    enqueueCloseTask(std::move(task));
  }
  co_return result;
}

CoTask<void> MetaClient::pruneSession(SessionId session) {
  // file session may create on FoundationDB, prune session to avoid file session leak.
  XLOGF(INFO, "MetaClient add prune session task {}", session);
  auto sessions = batchPrune_.lock()->push(session, config_.background_closer().prune_session_batch_count());
  if (!sessions.empty()) {
    CloseTask task{PruneSessionReq(clientId_, std::move(sessions)), config_.background_closer().retry_first_wait()};
    co_await bgCloser_->enqueue(std::move(task));
  }
}

CoTryTask<Inode> MetaClient::tryClose(const CloseReq &req, bool &needRetry) {
  auto server = co_await getServerNode();
  CO_RETURN_ON_ERROR(server);
  auto options = net::UserRequestOptions();
  auto timestamp = serde::Timestamp();
  options.timeout = config_.retry_default().rpc_timeout();
  auto result = (co_await ((server->stub)->close(req, options, &timestamp))).then(EXTRACT(stat));
  RECORD_TIMESATAMP("close", result, timestamp);
  if (result.hasError()) {
    needRetry = req.session.has_value() && ErrorHandling::retryable(result.error());
    XLOGF(WARN, "MetaClient failed to close {}, error {}, needRetry {}", req, result.error(), needRetry);
  }
  co_return result;
}

CoTryTask<void> MetaClient::tryPrune(const PruneSessionReq &req, bool &needRetry) {
  assert(!req.sessions.empty());
  auto server = co_await getServerNode();
  CO_RETURN_ON_ERROR(server);
  auto options = net::UserRequestOptions();
  auto timestamp = serde::Timestamp();
  options.timeout = config_.retry_default().rpc_timeout();
  auto result = (co_await (server->stub->pruneSession(req, options, &timestamp))).then(RETURN_VOID);
  RECORD_TIMESATAMP("prune", result, timestamp);
  if (result.hasError()) {
    needRetry = true;
    XLOGF(WARN, "MetaClient failed to prune session {}, error {}, needRetry {}", req, result.error(), needRetry);
  } else {
    for (auto session : req.sessions) {
      XLOGF(INFO, "MetaClient prune session {} success.", session);
    }
  }
  co_return result;
}

CoTryTask<Inode> MetaClient::setPermission(const UserInfo &userInfo,
                                           InodeId parent,
                                           const std::optional<Path> &path,
                                           bool followLastSymlink,
                                           std::optional<Uid> uid,
                                           std::optional<Gid> gid,
                                           std::optional<Permission> perm,
                                           std::optional<IFlags> iflags) {
  auto req = SetAttrReq::setPermission(userInfo,
                                       PathAt(parent, path),
                                       followLastSymlink ? AtFlags(AT_SYMLINK_FOLLOW) : AtFlags(AT_SYMLINK_NOFOLLOW),
                                       uid,
                                       gid,
                                       perm,
                                       iflags);
  co_return (co_await retry(&IMetaServiceStub::setAttr, req)).then(EXTRACT(stat));
}

CoTryTask<Inode> MetaClient::setIFlags(const UserInfo &userInfo, InodeId inode, IFlags iflags) {
  auto req = SetAttrReq::setIFlags(userInfo, inode, iflags);
  co_return (co_await retry(&IMetaServiceStub::setAttr, req)).then(EXTRACT(stat));
}

CoTryTask<Inode> MetaClient::utimes(const UserInfo &userInfo,
                                    InodeId inodeId,
                                    const std::optional<Path> &path,
                                    bool followLastSymlink,
                                    std::optional<UtcTime> atime,
                                    std::optional<UtcTime> mtime) {
  auto req = SetAttrReq::utimes(userInfo,
                                PathAt(inodeId, path),
                                followLastSymlink ? AtFlags(AT_SYMLINK_FOLLOW) : AtFlags(AT_SYMLINK_NOFOLLOW),
                                atime,
                                mtime);
  co_return (co_await retry(&IMetaServiceStub::setAttr, req)).then(EXTRACT(stat));
}

CoTryTask<Inode> MetaClient::mkdirs(const UserInfo &userInfo,
                                    InodeId parent,
                                    const Path &path,
                                    Permission perm,
                                    bool recursive,
                                    std::optional<Layout> layout) {
  if (layout.has_value()) {
    CO_RETURN_ON_ERROR(co_await updateLayout(*layout));
  }
  auto req = MkdirsReq(userInfo, PathAt(parent, path), meta::Permission(perm & ALLPERMS), recursive, layout);
  co_return (co_await retry(&IMetaServiceStub::mkdirs, req)).then(EXTRACT(stat));
}

CoTryTask<Inode> MetaClient::symlink(const UserInfo &userInfo, InodeId parent, const Path &path, const Path &target) {
  auto req = SymlinkReq(userInfo, PathAt(parent, path), target);
  co_return (co_await retry(&IMetaServiceStub::symlink, req)).then(EXTRACT(stat));
}

CoTryTask<void> MetaClient::removeImpl(RemoveReq &req) {
  if (req.recursive) {
    auto res = co_await stat(req.user, req.path.parent, req.path.path, false);
    CO_RETURN_ON_ERROR(res);
    req.inodeId = res->id;
  }
  co_return (co_await retry(&IMetaServiceStub::remove, req)).then(RETURN_VOID);
}

CoTryTask<void> MetaClient::remove(const UserInfo &userInfo,
                                   InodeId parent,
                                   const std::optional<Path> &path,
                                   bool recursive) {
  auto req = RemoveReq(userInfo, PathAt(parent, path), AtFlags(0), recursive, false);
  co_return co_await removeImpl(req);
}

CoTryTask<void> MetaClient::unlink(const UserInfo &userInfo, InodeId parent, const Path &path) {
  auto req = RemoveReq(userInfo, PathAt(parent, path), AtFlags(0), false, true);
  co_return co_await removeImpl(req);
}

CoTryTask<void> MetaClient::rmdir(const UserInfo &userInfo,
                                  InodeId parent,
                                  const std::optional<Path> &path,
                                  bool recursive) {
  auto req = RemoveReq(userInfo, PathAt(parent, path), AtFlags(AT_REMOVEDIR), recursive, true);
  co_return co_await removeImpl(req);
}

CoTryTask<Inode> MetaClient::rename(const UserInfo &userInfo,
                                    InodeId srcParent,
                                    const Path &src,
                                    InodeId dstParent,
                                    const Path &dst,
                                    bool moveToTrash) {
  auto req = RenameReq(userInfo, PathAt(srcParent, src), PathAt(dstParent, dst), moveToTrash);
  if (moveToTrash) {
    auto res = co_await stat(req.user, srcParent, src, false);
    CO_RETURN_ON_ERROR(res);
    req.inodeId = res->id;
  }
  auto result = co_await retry(&IMetaServiceStub::rename, req);
  CO_RETURN_ON_ERROR(result);
  if (result->stat.has_value()) {
    co_return std::move(*result->stat);
  }
  // NOTE: for compatibility, this maybe not atomic
  XLOGF(WARN, "rename doesn't return inode, server maybe not updated, stat dst after rename!");
  co_return co_await stat(userInfo, dstParent, dst, false /* don't follow symlink */);
}

CoTryTask<ListRsp> MetaClient::list(const UserInfo &userInfo,
                                    InodeId inodeId,
                                    const std::optional<Path> &path,
                                    std::string_view prev,
                                    int32_t limit,
                                    bool needStatus) {
  auto req = ListReq(userInfo, PathAt(inodeId, path), std::string(prev), limit, needStatus);
  co_return co_await retry(&IMetaServiceStub::list, req);
}

CoTryTask<Inode> MetaClient::setLayout(const UserInfo &userInfo,
                                       InodeId inodeId,
                                       const std::optional<Path> &path,
                                       Layout layout) {
  auto req = SetAttrReq::setLayout(userInfo, PathAt(inodeId, path), AtFlags(), layout);
  co_return (co_await retry(&IMetaServiceStub::setAttr, req)).then(EXTRACT(stat));
}

#define CHECK_FILE(inode)                                                                                            \
  do {                                                                                                               \
    if (!inode.isFile()) {                                                                                           \
      co_return makeError(MetaCode::kNotFile, "Not file");                                                           \
    }                                                                                                                \
    if (auto valid = inode.asFile().layout.valid(false); valid.hasError()) {                                         \
      XLOGF(CRITICAL, "File {} has a invalid layout {}, error {}!", inode.id, inode.asFile().layout, valid.error()); \
      CO_RETURN_ERROR(valid);                                                                                        \
    }                                                                                                                \
  } while (0)

#define GET_RAW_ROUTING_INFO()                                  \
  auto routingInfo = mgmtd_->getRoutingInfo();                  \
  if (!routingInfo || !routingInfo->raw()) {                    \
    XLOGF(ERR, "RoutingInfo not ready");                        \
    co_return makeError(MgmtdClientCode::kRoutingInfoNotReady); \
  }                                                             \
  auto rawRoutingInfo = routingInfo->raw()

CoTryTask<Inode> MetaClient::truncateImpl(const UserInfo &userInfo, const Inode &inode, const size_t targetLength) {
  auto reqInfo = RequestInfo::get();
  if (!storage_) {
    XLOGF(DFATAL, "Storage client is not set!!!");
    co_return makeError(MetaCode::kFoundBug, "Storage client is not set");
  }

  CHECK_FILE(inode);
  GET_RAW_ROUTING_INFO();
  FileOperation fop(*storage_, *rawRoutingInfo, userInfo, inode, recorder);

  // remove chunks
  size_t iter = 0;
  SCOPE_EXIT { truncateIters.addSample(iter); };
  bool more = true;
  uint64_t removed = 0;
  while (more) {
    if (reqInfo && reqInfo->canceled()) {
      XLOGF(WARN, "Request canceled by {}, stop truncate", reqInfo->describe());
      co_return makeError(MetaCode::kRequestCanceled, "request canceled, stop truncate");
    }

    iter++;
    // if (config_.remove_chunks_max_iters() != 0 && iter > config_.remove_chunks_max_iters()) {
    //   auto msg = fmt::format("truncate {} to {}, current iter {} > max {}, removed {}",
    //                          inode.id,
    //                          targetLength,
    //                          iter,
    //                          config_.remove_chunks_max_iters(),
    //                          more);
    //   XLOG(ERR, msg);
    //   co_return makeError(MetaCode::kMoreChunksToRemove, msg);
    // }

    auto remove = co_await fop.removeChunks(targetLength,
                                            config_.remove_chunks_batch_size(),
                                            dynStripe_ && config_.dynamic_stripe(),
                                            {});
    CO_RETURN_ON_ERROR(remove);
    removed += remove->first;
    more = remove->second;
    XLOGF(INFO, "truncate {} to {}, removed {}, more {}", inode.id, targetLength, removed, more);
    if (remove->first == 0 && more) {
      auto msg =
          fmt::format("truncate {} to {}, removed 0 chunk in this iter, but has more to remove, total removed {}",
                      inode.id,
                      targetLength,
                      removed);
      XLOG(CRITICAL, msg);
      co_return makeError(MetaCode::kFoundBug, msg);
    }
  }

  auto stripe = std::min((uint32_t)folly::divCeil(targetLength, (uint64_t)inode.asFile().layout.chunkSize),
                         inode.asFile().layout.stripeSize);
  if (inode.asFile().dynStripe && inode.asFile().dynStripe < stripe) {
    CO_RETURN_ON_ERROR(co_await extendStripe(userInfo, inode.id, stripe));
  }

  // truncate last chunk
  CO_RETURN_ON_ERROR(co_await fop.truncateChunk(targetLength));

  // issue a sync request
  auto req = SyncReq(userInfo, inode.id, true, std::nullopt, std::nullopt, true /* truncated */);
  co_return (co_await retry(&IMetaServiceStub::sync, req)).then(EXTRACT(stat));
}

CoTryTask<Inode> MetaClient::truncate(const UserInfo &userInfo, InodeId inodeId, uint64_t length) {
  auto inode = co_await stat(userInfo, inodeId, std::nullopt, true);
  CO_RETURN_ON_ERROR(inode);
  co_return co_await truncateImpl(userInfo, *inode, length);
}

CoTryTask<Inode> MetaClient::sync(const UserInfo &userInfo,
                                  InodeId inode,
                                  bool readded,
                                  bool written,
                                  std::optional<VersionedLength> hint) {
  co_return co_await sync(userInfo, inode, written, OPTIONAL_TIME(readded), OPTIONAL_TIME(written), hint);
}

CoTryTask<Inode> MetaClient::sync(const UserInfo &userInfo,
                                  InodeId inode,
                                  bool updateLength,
                                  const std::optional<UtcTime> atime,
                                  const std::optional<UtcTime> mtime,
                                  std::optional<VersionedLength> hint) {
  auto req = SyncReq(userInfo, inode, updateLength, atime, mtime, false, hint);
  co_return (co_await retry(&IMetaServiceStub::sync, req)).then(EXTRACT(stat));
}

CoTryTask<Inode> MetaClient::hardLink(const UserInfo &userInfo,
                                      InodeId oldParent,
                                      const std::optional<Path> &oldPath,
                                      InodeId newParent,
                                      const Path &newPath,
                                      bool followLastSymlink) {
  auto req = HardLinkReq(userInfo,
                         PathAt(oldParent, oldPath),
                         PathAt(newParent, newPath),
                         followLastSymlink ? AtFlags(AT_SYMLINK_FOLLOW) : AtFlags(AT_SYMLINK_NOFOLLOW));
  co_return (co_await retry(&IMetaServiceStub::hardLink, req)).then(EXTRACT(stat));
}

CoTryTask<Inode> MetaClient::extendStripe(const UserInfo &userInfo, InodeId inodeId, uint32_t stripe) {
  auto req = SetAttrReq::extendStripe(userInfo, inodeId, stripe);
  co_return (co_await retry(&IMetaServiceStub::setAttr, req)).then(EXTRACT(stat));
}

CoTryTask<Void> MetaClient::testRpc() {
  co_return (co_await retry(&IMetaServiceStub::testRpc, TestRpcReq{{}, PathAt("/test-rpc/path/dir1/dir2/file/file3")}))
      .then(RETURN_VOID);
}

CoTryTask<void> MetaClient::lockDirectory(const UserInfo &userInfo,
                                          InodeId inode,
                                          LockDirectoryReq::LockAction action) {
  co_return (co_await retry(&IMetaServiceStub::lockDirectory, LockDirectoryReq{userInfo, inode, action}))
      .then(RETURN_VOID);
}

}  // namespace hf3fs::meta::client
