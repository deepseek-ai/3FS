#include "MgmtdHeartbeater.h"

#include "common/app/ApplicationBase.h"
#include "core/utils/ServiceOperation.h"
#include "core/utils/runOp.h"
#include "mgmtd/service/MgmtdState.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
struct MgmtdHeartbeater::SendHeartbeatContext {
  flat::MgmtdLeaseInfo lease;
  std::unique_ptr<MgmtdStub> stub;
  flat::HeartbeatInfo info;
};

namespace {
struct Op : core::ServiceOperationWithMetric<"MgmtdService", "SendHeartbeat", "bg"> {
  String toStringImpl() const final { return "SendHeartbeat"; }

  auto handle(MgmtdState &state, MgmtdHeartbeater::SendHeartbeatContext &sendHeartbeatCtx) -> CoTryTask<void> {
    auto &info = sendHeartbeatCtx.info;
    info.configStatus = ApplicationBase::getConfigStatus();
    auto res = co_await sendHeartbeatCtx.stub->heartbeat(
        HeartbeatReq::create(state.env_->appInfo().clusterId, info, UtcClock::now()));
    if (res.hasError()) {
      if (res.error().code() == MgmtdCode::kHeartbeatVersionStale) {
        uint64_t v = 0;
        auto result = scn::scan(String(res.error().message()), "{}", v);
        if (result) {
          info.hbVersion = flat::HeartbeatVersion(v + 1);
        } else {
          info.hbVersion = flat::HeartbeatVersion(info.hbVersion + 1);
        }
      }
      CO_RETURN_ERROR(res);
    } else {
      if (res->config) {
        auto r = updateSelfConfig(state, *res->config);
        if (r.hasError()) {
          LOG_OP_WARN(*this, "Update config failed: {}. version: {}", r.error(), res->config->configVersion);
        } else {
          info.configVersion = res->config->configVersion;
          LOG_OP_INFO(*this, "Update config succeeded and promote config version to {}", res->config->configVersion);
        }
      }
      info.hbVersion = nextVersion(info.hbVersion);
      co_return Void{};
    }
  }
};
}  // namespace

MgmtdHeartbeater::MgmtdHeartbeater(MgmtdState &state)
    : state_(state) {}

MgmtdHeartbeater::~MgmtdHeartbeater() {}

CoTask<void> MgmtdHeartbeater::send() {
  Op op;

  if (!state_.config_.send_heartbeat()) {
    sendHeartbeatCtx_.reset();
    LOG_OP_DBG(op, "disabled, skip");
    co_return;
  }
  auto start = state_.utcNow();
  auto lease = co_await state_.currentLease(start);
  if (!lease.has_value()) {
    sendHeartbeatCtx_.reset();
    LOG_OP_DBG(op, "primary not found, skip");
    co_return;
  } else if (lease->primary.nodeId == state_.selfId()) {
    sendHeartbeatCtx_.reset();
    LOG_OP_DBG(op, "self is primary, skip");
    co_return;
  }

  if (!sendHeartbeatCtx_ || sendHeartbeatCtx_->lease.leaseStart != lease->leaseStart) {
    auto addrs = flat::extractAddresses(lease->primary.serviceGroups, "Mgmtd");
    if (addrs.empty()) {
      LOG_OP_ERR(op, "primary({}) addr not found, skip", lease->primary.nodeId);
      co_return;
    }
    sendHeartbeatCtx_ = std::make_unique<SendHeartbeatContext>();
    sendHeartbeatCtx_->lease = *lease;
    // TODO: consider reuse some facilities of MgmtdClient for auto switching addresses
    sendHeartbeatCtx_->stub = state_.env_->mgmtdStubFactory()->create(addrs[0]);
    sendHeartbeatCtx_->info = flat::HeartbeatInfo(state_.env_->appInfo(), flat::MgmtdHeartbeatInfo{});
  }

  co_await [&]() -> CoTryTask<void> { CO_INVOKE_OP_INFO(op, "background", state_, *sendHeartbeatCtx_); }();
}
}  // namespace hf3fs::mgmtd
