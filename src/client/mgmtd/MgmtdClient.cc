#include "MgmtdClient.h"

#include <folly/Overload.h>
#include <folly/Synchronized.h>
#include <folly/concurrency/AtomicSharedPtr.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/BoundedQueue.h>
#include <folly/experimental/coro/FutureUtil.h>
#include <folly/experimental/coro/Promise.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <scn/scn.h>

#include "common/app/ApplicationBase.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/LogCommands.h"
#include "core/utils/ServiceOperation.h"
#include "core/utils/runOp.h"

namespace hf3fs::client {
namespace {
using namespace std::chrono_literals;

status_code_t kSkipThisNode = 65535;

using MgmtdStub = MgmtdClient::MgmtdStub;
using MgmtdStubFactory = MgmtdClient::MgmtdStubFactory;
using Impl = MgmtdClient::Impl;
using Callable = std::function<CoTask<void>(Impl &)>;

bool isNetworkError(status_code_t code) { return StatusCode::typeOf(code) == StatusCodeType::RPC; }

template <typename T, typename Value>
struct WorkItemBase {
  std::optional<folly::coro::Promise<Result<Value>>> promise;

  static auto makeItemWithWaiter(auto &&...args) {
    T item(std::forward<decltype(args)>(args)...);
    auto [promise, future] = folly::coro::makePromiseContract<Result<Value>>();
    item.promise = std::move(promise);
    return std::make_pair(std::move(item), folly::coro::toTask(std::move(future)));
  }

  static auto makeItem(auto &&...args) { return T(std::forward<decltype(args)>(args)...); }
};

struct ExitWorkItem : WorkItemBase<ExitWorkItem, Void> {};

struct RefreshWorkItem : WorkItemBase<RefreshWorkItem, Void> {
  bool force = false;

  explicit RefreshWorkItem(bool f)
      : force(f) {}
};

struct HeartbeatWorkItem : WorkItemBase<HeartbeatWorkItem, mgmtd::HeartbeatRsp> {};

struct CallableWorkItem : WorkItemBase<CallableWorkItem, Void> {
  std::string_view methodName;
  Callable callable;

  CallableWorkItem(std::string_view name, Callable c)
      : methodName(name),
        callable(std::move(c)) {}
};

struct WorkItem {
  std::variant<ExitWorkItem, RefreshWorkItem, HeartbeatWorkItem, CallableWorkItem> variant;

  template <typename WI>
  static auto makeItemWithWaiter(auto &&...args) {
    auto [wi, waitItem] = WI::makeItemWithWaiter(std::forward<decltype(args)>(args)...);
    auto item = std::make_unique<WorkItem>();
    item->variant = std::move(wi);
    return std::make_pair(std::move(item), std::move(waitItem));
  }

  template <typename WI>
  static auto makeItem(auto &&...args) {
    auto wi = WI::makeItem(std::forward<decltype(args)>(args)...);
    auto item = std::make_unique<WorkItem>();
    item->variant = std::move(wi);
    return std::move(item);
  }
};

struct Conn {
  flat::NodeId nodeId{0};
  std::vector<net::Address> addrs;

  net::Address addr() const {
    assert(addrIdx_ < addrs.size());
    return addrs[addrIdx_];
  }

  MgmtdStub &stub(MgmtdStubFactory &factory) {
    if (!stub_) {
      stub_ = factory.create(addr());
    }
    return *stub_;
  }

  void switchAddr() {
    stub_.reset();
    addrIdx_ = (addrIdx_ + 1) % addrs.size();
  }

 private:
  size_t addrIdx_{0};
  std::unique_ptr<MgmtdStub> stub_;
};

}  // namespace

struct MgmtdClient::Impl {
  struct ProbeContext {
    robin_hood::unordered_set<net::Address> probedAddrs;

    bool probed(net::Address addr) const { return probedAddrs.contains(addr); }
  };

  CoTryTask<void> retryOnError(ProbeContext &probeContext, Conn &conn, Status &error) {
    auto switchRes = co_await trySwitchProbeTarget(probeContext, conn, error);
    if (switchRes.hasError() && switchRes.error().code() != kSkipThisNode) CO_RETURN_ERROR(switchRes);
    if (!switchRes.hasError()) {
      if (*switchRes == conn.nodeId) co_return Void{};
      primaryMgmtdId_ = *switchRes;
    }
    auto primaryRes = co_await probePrimary(probeContext);
    if (!primaryRes.hasError() && primaryRes->has_value()) {
      primaryMgmtdId_ = primaryRes->value();
      co_return Void{};
    }
    co_return makeError(MgmtdClientCode::kPrimaryMgmtdNotFound);
  }

  CoTryTask<flat::NodeId> trySwitchProbeTarget(ProbeContext &probeContext, Conn &conn, Status &error) {
    auto code = error.code();
    if (isNetworkError(code) || code == MgmtdCode::kNotPrimary) {
      if (code == MgmtdCode::kNotPrimary) {
        XLOGF(INFO, "MgmtdClient: mark node as NotPrimary {} {}", conn.nodeId, serde::toJsonString(conn.addrs));
        probeContext.probedAddrs.insert(conn.addrs.begin(), conn.addrs.end());

        uint32_t id = 0;
        auto result = scn::scan(String(error.message()), "{}", id);
        if (result) {
          if (mgmtds_.contains(flat::NodeId(id))) {
            XLOGF(INFO, "MgmtdClient: found potential primary: {}", flat::NodeId(id));
            co_return flat::NodeId(id);
          } else {
            XLOGF(WARN, "MgmtdClient: found unknown potential primary: {}", flat::NodeId(id));
          }
        }
        co_return makeError(kSkipThisNode);
      } else {
        probeContext.probedAddrs.insert(conn.addr());
        conn.switchAddr();
        if (!probeContext.probed(conn.addr())) {
          co_return conn.nodeId;
        }
        XLOGF(INFO, "MgmtdClient: mark node as Skipped {} {}", conn.nodeId, serde::toJsonString(conn.addrs));
        probeContext.probedAddrs.insert(conn.addrs.begin(), conn.addrs.end());
        co_return makeError(kSkipThisNode);
      }
    }
    co_return makeError(std::move(error));
  }

  template <typename F>
  auto withRetry(std::string_view methodName, F &&f) -> std::invoke_result_t<F, MgmtdStub &> {
    ProbeContext probeContext;
    if (primaryMgmtdId_ == 0) CO_RETURN_ON_ERROR(co_await connect(probeContext));

    for (bool retried = false;;) {
      auto &conn = getMgmtdConn(primaryMgmtdId_);
      auto &stub = conn.stub(*mgmtdStubFactory_);
      if (retried)
        XLOGF(INFO, "MgmtdClient: retry {} via {} {}", methodName, conn.nodeId, conn.addr());
      else
        XLOGF(DBG3, "MgmtdClient: execute {} via {} {}", methodName, conn.nodeId, conn.addr());
      auto res = co_await f(stub);
      LOG_RESULT(INFO, res, "MgmtdClient: {}", methodName);
      if (!res.hasError() || retried) co_return res;

      CO_RETURN_ON_ERROR(co_await retryOnError(probeContext, conn, res.error()));
      retried = true;
    }
  }

  Impl(String clusterId, std::unique_ptr<MgmtdStubFactory> stubFactory, const Config &config)
      : clusterId_(std::move(clusterId)),
        mgmtdStubFactory_(std::move(stubFactory)),
        config_(config),
        workItems_(config_.work_queue_size()) {
    assert(StatusCode::typeOf(kSkipThisNode) == StatusCodeType::Invalid);
  }

  ~Impl() { folly::coro::blockingWait(stop()); }

  template <typename Op>
  auto invoke(Op &op) -> CoTryTask<typename Op::ResType> {
    auto handler = [&](Impl &impl) -> CoTryTask<typename Op::ResType> { CO_INVOKE_OP_INFO(op, "", impl); };
    Result<typename Op::ResType> res = makeError(StatusCode::kUnknown);
    auto callable = [&](Impl &impl) -> CoTask<void> { res = co_await handler(impl); };
    CO_RETURN_ON_ERROR(co_await pushCallable(op.methodName(), std::move(callable)));
    co_return res;
  }

  CoTryTask<void> pushCallable(std::string_view methodName, Callable callable) {
    auto [item, waitItem] = WorkItem::makeItemWithWaiter<CallableWorkItem>(methodName, std::move(callable));
    CO_RETURN_ON_ERROR(tryEnqueue(std::move(item)));
    co_return co_await std::move(waitItem);
  }

  CoTask<void> start(folly::Executor *backgroundExecutor, bool startBackground) {
    auto lock = std::unique_lock(backgroundRunningMu);
    if (backgroundRunning == false) {
      if (!backgroundExecutor) backgroundExecutor = co_await folly::coro::co_current_executor;
      backgroundExecutor_ = backgroundExecutor;
      actor().scheduleOn(backgroundExecutor).start();
      XLOGF(INFO, "MgmtdClient: started");
    }

    if (startBackground && !backgroundRunner_) {
      startBackgroundTasksWithLock();
    }

    backgroundRunning = true;
  }

  CoTask<void> startBackgroundTasks() {
    auto lock = std::unique_lock(backgroundRunningMu);
    if (backgroundRunning == true && !backgroundRunner_) {
      startBackgroundTasksWithLock();
    }
    co_return;
  }

  void startBackgroundTasksWithLock() {
    assert(!backgroundRunner_);
    backgroundRunner_ = std::make_unique<BackgroundRunner>(backgroundExecutor_);
    if (config_.enable_auto_refresh()) {
      backgroundRunner_->start(
          "AutoRefresh",
          [this] { return autoRefresh(); },
          config_.auto_refresh_interval_getter());
    }
    if (config_.enable_auto_heartbeat()) {
      backgroundRunner_->start(
          "AutoHeartbeat",
          [this] { return autoHeartbeat(); },
          config_.auto_heartbeat_interval_getter());
    }
    if (config_.enable_auto_extend_client_session()) {
      backgroundRunner_->start(
          "AutoExtendClientSession",
          [this] { return autoExtendClientSession(); },
          config_.auto_extend_client_session_interval_getter());
    }
  }

  CoTask<void> stop() {
    {
      auto lock = std::unique_lock(backgroundRunningMu);
      if (backgroundRunning == false) co_return;
      backgroundRunning = false;
    }
    auto [item, waitItem] = WorkItem::makeItemWithWaiter<ExitWorkItem>();
    co_await workItems_.enqueue(std::move(item));
    XLOGF(INFO, "MgmtdClient: wait for stop");
    co_await std::move(waitItem);
    if (backgroundRunner_) {
      co_await backgroundRunner_->stopAll();
      backgroundRunner_.reset();
    }
    XLOGF(INFO, "MgmtdClient: stopped");
  }

  CoTask<void> autoRefresh() {
    if (!config_.enable_auto_refresh()) {
      XLOGF(INFO, "MgmtdClient: AutoRefresh is disabled, skip");
      co_return;
    }
    auto res = co_await refreshRoutingInfo(/*force=*/false);
    LOG_RESULT(DBG, res, "MgmtdClient: AutoRefresh");

    if (res.hasError() && res.error().code() == MgmtdClientCode::kExit) {
      co_return;
    }
  }

  CoTask<void> autoHeartbeat() {
    if (!config_.enable_auto_heartbeat()) {
      XLOGF(INFO, "MgmtdClient: AutoHeartbeat is disabled, skip");
      co_return;
    }
    auto res = co_await heartbeat();
    LOG_RESULT(DBG, res, "MgmtdClient: AutoHeartbeat");

    if (res.hasError() && res.error().code() == MgmtdClientCode::kExit) {
      co_return;
    }
  }

  CoTask<void> autoExtendClientSession() {
    if (!config_.enable_auto_extend_client_session()) {
      XLOGF(INFO, "MgmtdClient: AutoExtendClientSession is disabled, skip");
      co_return;
    }
    ExtendClientSessionOp op;
    auto res = co_await invoke(op);
    LOG_RESULT(DBG, res, "MgmtdClient: AutoExtendClientSession");

    if (res.hasError() && res.error().code() == MgmtdClientCode::kExit) {
      co_return;
    }
  }

  struct ExtendClientSessionOp : core::ServiceOperationWithMetric<"MgmtdClient", "ExtendClientSession", "op"> {
    using ResType = Void;

    std::string_view methodName() const { return "ExtendClientSession"; }
    String toStringImpl() const final { return "ExtendClientSession"; }

    CoTryTask<Void> handle(Impl &impl) {
      auto payload = impl.clientSessionPayload_.load(std::memory_order_acquire);
      if (!payload) {
        XLOGF(ERR, "MgmtdClient: ClientSessionPayload not set, can't extend ClientSession");
        co_return makeError(StatusCode::kInvalidArg, "ClientSessionPayload not set");
      }
      if (!impl.clientSessionReq_ || impl.clientSessionReq_->clientId != payload->clientId ||
          impl.clientSessionReq_->data != payload->data) {
        auto req = std::make_unique<mgmtd::ExtendClientSessionReq>();
        req->clusterId = impl.clusterId_;
        req->clientId = payload->clientId;
        req->data = payload->data;
        req->type = payload->nodeType;
        req->user = payload->userInfo;
        req->clientStart = impl.startTime_;
        impl.clientSessionReq_ = std::move(req);
      }
      impl.clientSessionReq_->configStatus = ApplicationBase::getConfigStatus();

      auto res = co_await impl.withRetry("ExtendClientSession",
                                         [&](MgmtdStub &stub) -> CoTryTask<mgmtd::ExtendClientSessionRsp> {
                                           co_return co_await stub.extendClientSession(*impl.clientSessionReq_);
                                         });

      if (res.hasError()) {
        if (res.error().code() == MgmtdCode::kClientSessionVersionStale) {
          uint32_t v = 0;
          auto result = scn::scan(String(res.error().message()), "{}", v);
          if (result) {
            impl.clientSessionReq_->clientSessionVersion = flat::ClientSessionVersion(v + 1);
          } else {
            ++impl.clientSessionReq_->clientSessionVersion;
          }
        }
        CO_RETURN_ERROR(res);
      } else {
        ++impl.clientSessionReq_->clientSessionVersion;

        if (res->config) {
          auto configListener = impl.clientConfigListener_.load(std::memory_order_acquire);
          if (!configListener || (*configListener)(res->config->content, res->config->genUpdateDesc())) {
            if (!configListener) {
              XLOGF(WARN, "MgmtdClient: discard new config since no listener found");
            }
            impl.clientSessionReq_->configVersion = res->config->configVersion;
          }
        }
      }
      co_return Void{};
    }
  };

  CoTask<void> actor() {
    auto handler = folly::overload(
        [&](ExitWorkItem &item) -> CoTask<bool> {
          XLOGF(INFO, "MgmtdClient: receive ExitItem");
          if (item.promise) {
            item.promise->setValue(Void{});
          }
          co_return true;
        },
        [&](RefreshWorkItem &item) -> CoTask<bool> {
          XLOGF(DBG, "MgmtdClient: receive RefreshItem force={}", item.force);
          RefreshOp op(item.force);
          auto res = co_await [&]() -> CoTryTask<void> { CO_INVOKE_OP_INFO(op, "", *this); }();
          if (item.promise) {
            item.promise->setValue(std::move(res));
          }
          co_return false;
        },
        [&](HeartbeatWorkItem &item) -> CoTask<bool> {
          XLOGF(DBG, "MgmtdClient: receive HeartbeatItem");
          HeartbeatOp op;
          auto res = co_await [&]() -> CoTryTask<mgmtd::HeartbeatRsp> { CO_INVOKE_OP_INFO(op, "", *this); }();
          if (item.promise) {
            item.promise->setValue(std::move(res));
          }
          co_return false;
        },
        [&](CallableWorkItem &item) -> CoTask<bool> {
          XLOGF(DBG, "MgmtdClient: receive CallableWorkItem {}", item.methodName);
          co_await item.callable(*this);
          if (item.promise) {
            item.promise->setValue(Void{});
          }
          co_return false;
        });
    for (;;) {
      auto item = co_await workItems_.dequeue();
      auto exitTag = co_await std::visit(handler, item->variant);
      if (exitTag) {
        break;
      }
    }
  }

  CoTryTask<void> connect(ProbeContext &probeContext) {
    CO_RETURN_ON_ERROR(initMgmtds());
    XLOGF(INFO, "MgmtdClient: start probe for connecting ...");
    auto primaryRes = co_await probePrimary(probeContext);
    if (!primaryRes.hasError() && primaryRes->has_value()) {
      primaryMgmtdId_ = primaryRes->value();
      XLOGF(INFO, "MgmtdClient: found new primary {}", primaryMgmtdId_);
      co_return Void{};
    } else {
      if (primaryRes.hasError()) {
        XLOGF(ERR, "MgmtdClient: probePrimary failed: {}", primaryRes.error());
      } else {
        XLOGF(WARN, "MgmtdClient: primary not found");
      }
      co_return makeError(MgmtdClientCode::kPrimaryMgmtdNotFound);
    }
  }

  std::shared_ptr<RoutingInfo> getRoutingInfo() { return routingInfo_.load(std::memory_order_acquire); }

  Result<Void> tryEnqueue(std::unique_ptr<WorkItem> item) {
    auto lock = std::unique_lock(backgroundRunningMu);
    if (!backgroundRunning) {
      return makeError(MgmtdClientCode::kExit);
    }
    if (!workItems_.try_enqueue(std::move(item))) {
      return makeError(MgmtdClientCode::kWorkQueueFull);
    }
    return Void{};
  }

  CoTryTask<void> refreshRoutingInfo(bool force) {
    auto [item, waitItem] = WorkItem::makeItemWithWaiter<RefreshWorkItem>(force);
    CO_RETURN_ON_ERROR(tryEnqueue(std::move(item)));
    co_return co_await std::move(waitItem);
  }

  Result<Void> initMgmtds() {
    const auto &serverAddrs = config_.mgmtd_server_addresses();
    if (serverAddrs.empty()) {
      return makeError(StatusCode::kInvalidConfig, "Empty mgmtdServers");
    }
    for (auto addr : serverAddrs) {
      if (addr == net::Address(0))
        return makeError(StatusCode::kInvalidConfig, "Invalid MGMTD address: " + addr.toString());
      if (config_.network_type() && addr.type != *config_.network_type()) {
        return makeError(StatusCode::kInvalidConfig,
                         fmt::format("Invalid MGMTD address type: {}. expected {}",
                                     addr.toString(),
                                     magic_enum::enum_name(*config_.network_type())));
      }
      addrMap_.try_emplace(addr, flat::NodeId(0));  // 0 denotes "UNKNOWN"
    }
    return Void{};
  }

  // probe primary in DFS from `[id, conn]`
  // NOTE: callers must handle kSkipThisNode.
  CoTryTask<std::optional<flat::NodeId>> probePrimary(ProbeContext &probeContext,
                                                      Conn &conn,
                                                      int probeChainLength = 0) {
    // TODO: avoid hard code
    if (probeChainLength > 3) {
      XLOGF(ERR, "MgmtdClient: probePrimary stopped due to too long probeChain({})", probeChainLength);
      co_return makeError(kSkipThisNode);
    }

    Result<mgmtd::GetPrimaryMgmtdRsp> primaryRes = mgmtd::GetPrimaryMgmtdRsp::create(std::nullopt);
    auto req = mgmtd::GetPrimaryMgmtdReq::create(clusterId_);
    for (;;) {
      if (probeContext.probed(conn.addr())) {
        conn.switchAddr();
        if (probeContext.probed(conn.addr())) {
          XLOGF(WARN,
                "MgmtdClient: probePrimary mark node as Skipped {} {}",
                conn.nodeId,
                serde::toJsonString(conn.addrs));
          probeContext.probedAddrs.insert(conn.addrs.begin(), conn.addrs.end());
          co_return makeError(kSkipThisNode);
        }
      }

      XLOGF(INFO, "MgmtdClient: probePrimary {} via {} ...", conn.nodeId, conn.addr());
      auto &stub = conn.stub(*mgmtdStubFactory_);
      primaryRes = co_await stub.getPrimaryMgmtd(req);
      if (!primaryRes.hasError()) break;

      XLOGF(ERR, "MgmtdClient: probePrimary {} failed: {}", conn.nodeId, primaryRes.error());
      auto switchRes = co_await trySwitchProbeTarget(probeContext, conn, primaryRes.error());
      CO_RETURN_ON_ERROR(switchRes);
      if (*switchRes == conn.nodeId) continue;
      co_return co_await probePrimary(probeContext, getMgmtdConn(*switchRes), probeChainLength + 1);
    }

    if (!primaryRes->primary.has_value()) {
      XLOGF(WARN, "MgmtdClient: probePrimary {} stopped due to no primary", conn.nodeId);
      co_return std::nullopt;
    }

    const auto &primary = *primaryRes->primary;

    if (!tryAddMgmtd(primary.nodeId, primary.serviceGroups)) {
      XLOGF(WARN,
            "MgmtdClient: probePrimary {} stopped due to unexpected node info: {}",
            conn.nodeId,
            serde::toJsonString(primary));
      co_return std::nullopt;
    }

    if (addrMap_[conn.addr()] == primary.nodeId) {
      XLOGF(INFO, "MgmtdClient: probePrimary succeeded at {}", primary.nodeId);
      co_return primary.nodeId;
    }

    co_return co_await probePrimary(probeContext, getMgmtdConn(primary.nodeId), probeChainLength + 1);
  }

  CoTryTask<std::optional<flat::NodeId>> probeUnknownAddrs(ProbeContext &probeContext) {
    for (auto [addr, nodeId] : addrMap_) {
      if (nodeId == flat::NodeId(0)) {  // only probe unknown addrs
        XLOGF(INFO, "MgmtdClient: start probe from unknown address {}", addr);
        auto res = co_await probePrimary(addr);
        if (res.hasError() && res.error().code() == kSkipThisNode) continue;
        co_return res;
      }
    }
    co_return std::nullopt;
  }

  Conn &getMgmtdConn(flat::NodeId id) {
    auto &conn = mgmtds_[id];
    XLOGF_IF(DFATAL, conn.nodeId != id, "Invalid conn: expected NodeId = {}. actual = {}", id, conn.nodeId);
    return conn;
  }

  // probe primary in DFS from current primary or from all known mgmtds
  CoTryTask<std::optional<flat::NodeId>> probePrimary(ProbeContext &probeContext) {
    if (primaryMgmtdId_ != 0) {
      auto &conn = getMgmtdConn(primaryMgmtdId_);
      XLOGF(INFO,
            "MgmtdClient: start probe from potential primary {} {}",
            conn.nodeId,
            serde::toJsonString(conn.addrs));
      auto res = co_await probePrimary(probeContext, conn);
      if (!res.hasError() || res.error().code() != kSkipThisNode) co_return res;
    }
    for (auto &[id, conn] : mgmtds_) {
      XLOGF(INFO, "MgmtdClient: start probe from mgmtd {} {}", conn.nodeId, serde::toJsonString(conn.addrs));
      auto res = co_await probePrimary(probeContext, conn);
      if (!res.hasError() || res.error().code() != kSkipThisNode) co_return res;
    }
    co_return co_await probeUnknownAddrs(probeContext);
  }

  // probe primary in DFS from an address
  CoTryTask<std::optional<flat::NodeId>> probePrimary(net::Address addr) {
    ProbeContext probeContext;
    Conn conn;
    conn.addrs.push_back(addr);
    co_return co_await probePrimary(probeContext, conn);
  }

  void addMgmtd(flat::NodeId nodeId, std::vector<net::Address> addrs) {
    XLOGF(INFO, "MgmtdClient: found new mgmtd {} {}", nodeId, fmt::join(addrs, ","));
    for (auto addr : addrs) addrMap_[addr] = nodeId;
    Conn conn;
    conn.nodeId = nodeId;
    conn.addrs = std::move(addrs);
    mgmtds_[nodeId] = std::move(conn);
  }

  bool tryAddMgmtd(flat::NodeId nodeId, const std::vector<flat::ServiceGroupInfo> &serviceGroups) {
    if (mgmtds_.contains(nodeId)) return true;
    auto addresses = flat::extractAddresses(serviceGroups, "Mgmtd", config_.network_type());
    if (addresses.empty()) return false;
    addMgmtd(nodeId, std::move(addresses));
    return true;
  }

  CoTryTask<void> refreshRoutingInfoImpl(bool force) {
    auto currentInfo = getRoutingInfo();
    auto curRoutingInfo = currentInfo ? currentInfo->raw() : nullptr;
    auto currentVersion = curRoutingInfo ? curRoutingInfo->routingInfoVersion : flat::RoutingInfoVersion(0);

    auto res = co_await withRetry("RefreshRoutingInfo", [&](MgmtdStub &stub) -> CoTryTask<mgmtd::GetRoutingInfoRsp> {
      co_return co_await stub.getRoutingInfo(
          mgmtd::GetRoutingInfoReq::create(clusterId_, force ? flat::RoutingInfoVersion{0} : currentVersion));
    });
    if (!res.hasError()) {
      if (res->info) {
        XLOGF(INFO, "MgmtdClient: get new routing info version {}", res->info->routingInfoVersion.toUnderType());
      }
      XLOGF(DBG, "MgmtdClient: get new routing info {}", serde::toJsonString(res->info));
      auto newInfo = res->info ? std::make_shared<flat::RoutingInfo>(std::move(*res->info)) : nullptr;
      updateRoutingInfo(std::move(newInfo), std::move(curRoutingInfo), currentVersion);
      co_return Void{};
    }
    CO_RETURN_ERROR(res);
  }

  void updateRoutingInfo(std::shared_ptr<flat::RoutingInfo> newRoutingInfo,
                         std::shared_ptr<flat::RoutingInfo> curRoutingInfo,
                         flat::RoutingInfoVersion currentVersion) {
    if (newRoutingInfo) {
      XLOGF_IF(FATAL,
               currentVersion > newRoutingInfo->routingInfoVersion,
               "RoutingInfoVersion rollback from {} to {}",
               currentVersion.toUnderType(),
               newRoutingInfo->routingInfoVersion.toUnderType());

      std::vector<flat::NodeId> connectingNodes;
      for ([[maybe_unused]] const auto &[id, node] : newRoutingInfo->nodes) {
        if (node.status == flat::NodeStatus::HEARTBEAT_CONNECTING) {
          connectingNodes.push_back(id);
        }
        tryAddMgmtd(node.app.nodeId, node.app.serviceGroups);
      }
      if (!config_.accept_incomplete_routing_info_during_mgmtd_bootstrapping() && newRoutingInfo->bootstrapping &&
          !connectingNodes.empty()) {
        // ignore incomplete RoutingInfo
        XLOGF(INFO,
              "MgmtdClient: discard incomplete routing info version {}, [{}] still connecting",
              newRoutingInfo->routingInfoVersion,
              fmt::join(connectingNodes, ","));
        return;
      }
    }
    auto ri = std::make_shared<RoutingInfo>(newRoutingInfo ? newRoutingInfo : curRoutingInfo, SteadyClock::now());
    routingInfo_.store(ri, std::memory_order_release);

    if (newRoutingInfo) {
      auto listenersPtr = routingInfoListeners_.rlock();
      for (const auto &[_, listener] : *listenersPtr) {
        listener(ri);
      }
    }
  }

  CoTryTask<mgmtd::HeartbeatRsp> heartbeat() {
    auto [item, waitItem] = WorkItem::makeItemWithWaiter<HeartbeatWorkItem>();
    CO_RETURN_ON_ERROR(tryEnqueue(std::move(item)));
    co_return co_await std::move(waitItem);
  }

  Result<Void> triggerHeartbeat() {
    auto item = WorkItem::makeItem<HeartbeatWorkItem>();
    return tryEnqueue(std::move(item));
  }

  struct HeartbeatOp : core::ServiceOperationWithMetric<"MgmtdClient", "Heartbeat", "op"> {
    String toStringImpl() const final { return "Heartbeat"; }

    CoTryTask<mgmtd::HeartbeatRsp> handle(Impl &impl) { return impl.heartbeatImpl(/*retryable=*/true); }
  };

  struct RefreshOp : core::ServiceOperationWithMetric<"MgmtdClient", "RefreshRoutingInfo", "op"> {
    explicit RefreshOp(bool force)
        : force_(force) {}

    String toStringImpl() const final { return fmt::format("RefreshRoutingInfo force={}", force_); }

    CoTryTask<void> handle(Impl &impl) { return impl.refreshRoutingInfoImpl(force_); }

    bool force_;
  };

  CoTryTask<mgmtd::HeartbeatRsp> heartbeatImpl(bool retryable) {
    if (!heartbeatInfo_) {
      auto appInfo = appInfo_.load(std::memory_order_acquire);
      if (!appInfo) {
        XLOGF(ERR, "MgmtdClient: Heartbeat AppInfo not set, can't send heartbeat");
        co_return makeError(StatusCode::kInvalidArg, "AppInfo not set");
      }

      heartbeatInfo_ = std::make_unique<flat::HeartbeatInfo>(*appInfo);
    }
    auto payload = heartbeatPayload_.load(std::memory_order_acquire);
    if (!payload) {
      XLOGF(ERR, "MgmtdClient: Heartbeat Payload not set, can't send heartbeat");
      co_return makeError(StatusCode::kInvalidArg, "Payload not set");
    }
    heartbeatInfo_->set(*payload);
    heartbeatInfo_->configStatus = ApplicationBase::getConfigStatus();

    auto req = mgmtd::HeartbeatReq::create(clusterId_, *heartbeatInfo_, UtcClock::now());
    XLOGF(DBG3, "MgmtdClient: HeartbeatReq {:?}", req);

    auto res = co_await withRetry("Heartbeat", [&](MgmtdStub &stub) -> CoTryTask<mgmtd::HeartbeatRsp> {
      XLOGF(INFO, "MgmtdClient: Heartbeat send ...");
      co_return co_await stub.heartbeat(req);
    });

    if (res.hasError()) {
      XLOGF(ERR, "MgmtdClient: Heartbeat failed: {}", res.error());
      if (res.error().code() == MgmtdCode::kHeartbeatVersionStale) {
        uint64_t v = 0;
        auto result = scn::scan(String(res.error().message()), "{}", v);
        if (result) {
          heartbeatInfo_->hbVersion = flat::HeartbeatVersion(v + 1);
        } else {
          heartbeatInfo_->hbVersion = flat::HeartbeatVersion(heartbeatInfo_->hbVersion + 1);
        }
        if (retryable) {
          co_return co_await heartbeatImpl(/*retryable=*/false);
        }
      }
    } else {
      XLOGF(INFO, "MgmtdClient: Heartbeat succeeded");
      heartbeatInfo_->hbVersion = flat::HeartbeatVersion(heartbeatInfo_->hbVersion + 1);
      if (res->config) {
        auto listener = serverConfigListener_.load(std::memory_order_acquire);
        // do not update config version when listener return false
        if (!listener || (*listener)(res->config->content, fmt::format("{}", res->config->configVersion))) {
          if (!listener) {
            XLOGF(WARN, "MgmtdClient: discard new config since no listener found");
          }
          heartbeatInfo_->configVersion = res->config->configVersion;
        }
      }
    }
    co_return res;
  }

  UtcTime startTime_ = UtcClock::now();
  String clusterId_;
  std::unique_ptr<MgmtdStubFactory> mgmtdStubFactory_;
  const Config &config_;

  folly::atomic_shared_ptr<RoutingInfo> routingInfo_;
  folly::Synchronized<std::map<String, RoutingInfoListener, std::less<>>> routingInfoListeners_;
  folly::atomic_shared_ptr<ConfigListener> serverConfigListener_;
  folly::atomic_shared_ptr<flat::AppInfo> appInfo_;
  folly::atomic_shared_ptr<HeartbeatPayload> heartbeatPayload_;
  folly::atomic_shared_ptr<ClientSessionPayload> clientSessionPayload_;
  folly::atomic_shared_ptr<ConfigListener> clientConfigListener_;
  std::unique_ptr<mgmtd::ExtendClientSessionReq> clientSessionReq_;

  robin_hood::unordered_map<net::Address, flat::NodeId> addrMap_;
  robin_hood::unordered_map<flat::NodeId, Conn> mgmtds_;
  flat::NodeId primaryMgmtdId_{0};

  folly::coro::BoundedQueue<std::unique_ptr<WorkItem>, false, true> workItems_;

  std::unique_ptr<BackgroundRunner> backgroundRunner_;
  std::unique_ptr<flat::HeartbeatInfo> heartbeatInfo_;

  std::mutex backgroundRunningMu;
  bool backgroundRunning{false};
  folly::Executor *backgroundExecutor_{nullptr};
};

namespace {
#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, name, Name, id, reqtype, rsptype)             \
  struct Name##Op : core::ServiceOperationWithMetric<"MgmtdClient", #Name, "op"> {          \
    using ReqType = mgmtd::reqtype;                                                         \
    using ResType = mgmtd::rsptype;                                                         \
                                                                                            \
    template <typename... Args>                                                             \
    Name##Op(Args &&...args)                                                                \
        : req(ReqType::create(std::forward<Args>(args)...)) {}                              \
                                                                                            \
    std::string_view methodName() const { return #Name; }                                   \
    String toStringImpl() const final { return #Name; }                                     \
                                                                                            \
    CoTryTask<ResType> handle(Impl &impl) {                                                 \
      co_return co_await impl.withRetry(#Name, [&](MgmtdStub &stub) -> CoTryTask<ResType> { \
        co_return co_await stub.name(req);                                                  \
      });                                                                                   \
    }                                                                                       \
                                                                                            \
    ReqType req;                                                                            \
  };
#include "fbs/mgmtd/MgmtdServiceDef.h"
}  // namespace

MgmtdClient::MgmtdClient(String clusterId, std::unique_ptr<MgmtdStubFactory> stubFactory, const Config &config) {
  impl_ = std::make_unique<Impl>(std::move(clusterId), std::move(stubFactory), config);
}

MgmtdClient::~MgmtdClient() { impl_.reset(); }

CoTask<void> MgmtdClient::start(folly::Executor *backgroundExecutor, bool startBackground) {
  co_await impl_->start(backgroundExecutor, startBackground);
}

CoTask<void> MgmtdClient::startBackgroundTasks() { co_await impl_->startBackgroundTasks(); }

CoTask<void> MgmtdClient::stop() { co_await impl_->stop(); }

std::shared_ptr<RoutingInfo> MgmtdClient::getRoutingInfo() { return impl_->getRoutingInfo(); }

CoTryTask<void> MgmtdClient::refreshRoutingInfo(bool force) { co_return co_await impl_->refreshRoutingInfo(force); }

CoTryTask<mgmtd::HeartbeatRsp> MgmtdClient::heartbeat() { co_return co_await impl_->heartbeat(); }

Result<Void> MgmtdClient::triggerHeartbeat() { return impl_->triggerHeartbeat(); }

void MgmtdClient::setAppInfoForHeartbeat(flat::AppInfo info) {
  impl_->appInfo_.store(std::make_shared<flat::AppInfo>(std::move(info)), std::memory_order_release);
}

bool MgmtdClient::addRoutingInfoListener(String name, RoutingInfoListener listener) {
  auto mapPtr = impl_->routingInfoListeners_.wlock();
  return mapPtr->try_emplace(name, std::move(listener)).second;
}

bool MgmtdClient::removeRoutingInfoListener(std::string_view name) {
  auto mapPtr = impl_->routingInfoListeners_.wlock();
  auto it = mapPtr->find(name);
  if (it == mapPtr->end()) {
    return false;
  }
  mapPtr->erase(it);
  return true;
}

void MgmtdClient::setServerConfigListener(ConfigListener listener) {
  impl_->serverConfigListener_.store(std::make_shared<ConfigListener>(std::move(listener)), std::memory_order_release);
}

void MgmtdClient::setClientConfigListener(ConfigListener listener) {
  impl_->clientConfigListener_.store(std::make_shared<ConfigListener>(std::move(listener)), std::memory_order_release);
}

void MgmtdClient::updateHeartbeatPayload(HeartbeatPayload payload) {
  impl_->heartbeatPayload_.store(std::make_shared<HeartbeatPayload>(std::move(payload)), std::memory_order_release);
}

CoTryTask<mgmtd::SetChainsRsp> MgmtdClient::setChains(const flat::UserInfo &userInfo,
                                                      const std::vector<flat::ChainSetting> &chains) {
  SetChainsOp op(impl_->clusterId_, chains, userInfo);
  co_return co_await impl_->invoke(op);
}

CoTryTask<mgmtd::SetChainTableRsp> MgmtdClient::setChainTable(const flat::UserInfo &userInfo,
                                                              flat::ChainTableId tableId,
                                                              const std::vector<flat::ChainId> &chains,
                                                              const String &desc) {
  SetChainTableOp op(impl_->clusterId_, tableId, chains, desc, userInfo);
  co_return co_await impl_->invoke(op);
}

CoTryTask<mgmtd::ListClientSessionsRsp> MgmtdClient::listClientSessions() {
  ListClientSessionsOp op(impl_->clusterId_);
  co_return co_await impl_->invoke(op);
}

CoTryTask<void> MgmtdClient::extendClientSession() {
  Impl::ExtendClientSessionOp op;
  co_return co_await impl_->invoke(op);
}

void MgmtdClient::setClientSessionPayload(ClientSessionPayload payload) {
  impl_->clientSessionPayload_.store(std::make_shared<ClientSessionPayload>(std::move(payload)),
                                     std::memory_order_release);
}

CoTryTask<flat::ConfigVersion> MgmtdClient::setConfig(const flat::UserInfo &userInfo,
                                                      flat::NodeType nodeType,
                                                      const String &content,
                                                      const String &desc) {
  SetConfigOp op(impl_->clusterId_, nodeType, content, desc, userInfo);
  auto res = co_await impl_->invoke(op);
  CO_RETURN_ON_ERROR(res);
  co_return res->configVersion;
}

CoTryTask<std::optional<flat::ConfigInfo>> MgmtdClient::getConfig(flat::NodeType nodeType,
                                                                  flat::ConfigVersion version) {
  GetConfigOp op(impl_->clusterId_, nodeType, version);
  auto res = co_await impl_->invoke(op);
  CO_RETURN_ON_ERROR(res);
  co_return std::move(res->info);
}

CoTryTask<RHStringHashMap<flat::ConfigVersion>> MgmtdClient::getConfigVersions() {
  GetConfigVersionsOp op(impl_->clusterId_);
  auto res = co_await impl_->invoke(op);
  CO_RETURN_ON_ERROR(res);
  co_return std::move(res->versions);
}

CoTryTask<void> MgmtdClient::enableNode(const flat::UserInfo &userInfo, flat::NodeId nodeId) {
  EnableNodeOp op(impl_->clusterId_, nodeId, userInfo);
  CO_RETURN_ON_ERROR(co_await impl_->invoke(op));
  co_return Void{};
}

CoTryTask<void> MgmtdClient::disableNode(const flat::UserInfo &userInfo, flat::NodeId nodeId) {
  DisableNodeOp op(impl_->clusterId_, nodeId, userInfo);
  CO_RETURN_ON_ERROR(co_await impl_->invoke(op));
  co_return Void{};
}

CoTryTask<void> MgmtdClient::registerNode(const flat::UserInfo &userInfo, flat::NodeId nodeId, flat::NodeType type) {
  RegisterNodeOp op(impl_->clusterId_, nodeId, type, userInfo);
  CO_RETURN_ON_ERROR(co_await impl_->invoke(op));
  co_return Void{};
}

CoTryTask<void> MgmtdClient::unregisterNode(const flat::UserInfo &userInfo, flat::NodeId nodeId) {
  UnregisterNodeOp op(impl_->clusterId_, nodeId, userInfo);
  CO_RETURN_ON_ERROR(co_await impl_->invoke(op));
  co_return Void{};
}

CoTryTask<flat::NodeInfo> MgmtdClient::setNodeTags(const flat::UserInfo &userInfo,
                                                   flat::NodeId nodeId,
                                                   const std::vector<flat::TagPair> &tags,
                                                   flat::SetTagMode mode) {
  SetNodeTagsOp op(impl_->clusterId_, nodeId, tags, mode, userInfo);
  auto res = co_await impl_->invoke(op);
  CO_RETURN_ON_ERROR(res);
  co_return std::move(res->info);
}

CoTryTask<std::vector<flat::TagPair>> MgmtdClient::setUniversalTags(const flat::UserInfo &userInfo,
                                                                    const String &universalId,
                                                                    const std::vector<flat::TagPair> &tags,
                                                                    flat::SetTagMode mode) {
  SetUniversalTagsOp op(impl_->clusterId_, universalId, tags, mode, userInfo);
  auto res = co_await impl_->invoke(op);
  CO_RETURN_ON_ERROR(res);
  co_return std::move(res->tags);
}

CoTryTask<std::vector<flat::TagPair>> MgmtdClient::getUniversalTags(const String &universalId) {
  GetUniversalTagsOp op(impl_->clusterId_, universalId);
  auto res = co_await impl_->invoke(op);
  CO_RETURN_ON_ERROR(res);
  co_return std::move(res->tags);
}

CoTryTask<mgmtd::GetClientSessionRsp> MgmtdClient::getClientSession(const String &clientId) {
  GetClientSessionOp op(impl_->clusterId_, clientId);
  co_return co_await impl_->invoke(op);
}

CoTryTask<flat::ChainInfo> MgmtdClient::rotateLastSrv(const flat::UserInfo &userInfo, flat::ChainId chainId) {
  RotateLastSrvOp op(impl_->clusterId_, chainId, userInfo);
  auto res = co_await impl_->invoke(op);
  CO_RETURN_ON_ERROR(res);
  co_return std::move(res->chain);
}

CoTryTask<flat::ChainInfo> MgmtdClient::rotateAsPreferredOrder(const flat::UserInfo &userInfo, flat::ChainId chainId) {
  RotateAsPreferredOrderOp op(impl_->clusterId_, chainId, userInfo);
  auto res = co_await impl_->invoke(op);
  CO_RETURN_ON_ERROR(res);
  co_return std::move(res->chain);
}

CoTryTask<flat::ChainInfo> MgmtdClient::setPreferredTargetOrder(
    const flat::UserInfo &userInfo,
    flat::ChainId chainId,
    const std::vector<flat::TargetId> &preferredTargetOrder) {
  SetPreferredTargetOrderOp op(impl_->clusterId_, chainId, preferredTargetOrder, userInfo);
  auto res = co_await impl_->invoke(op);
  CO_RETURN_ON_ERROR(res);
  co_return std::move(res->chain);
}

CoTryTask<mgmtd::ListOrphanTargetsRsp> MgmtdClient::listOrphanTargets() {
  ListOrphanTargetsOp op(impl_->clusterId_);
  co_return co_await impl_->invoke(op);
}

CoTryTask<flat::ChainInfo> MgmtdClient::updateChain(const flat::UserInfo &userInfo,
                                                    flat::ChainId cid,
                                                    flat::TargetId tid,
                                                    mgmtd::UpdateChainReq::Mode mode) {
  UpdateChainOp op(impl_->clusterId_, userInfo, cid, tid, mode);
  auto res = co_await impl_->invoke(op);
  CO_RETURN_ON_ERROR(res);
  co_return std::move(res->chain);
}
}  // namespace hf3fs::client
