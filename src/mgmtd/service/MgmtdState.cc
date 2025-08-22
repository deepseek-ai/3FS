#include "MgmtdState.h"

#include "MgmtdConfig.h"
#include "core/user/UserToken.h"
#include "fbs/mgmtd/NodeConversion.h"
#include "helpers.h"

namespace hf3fs::mgmtd {
namespace {
flat::NodeInfo initSelfNodeInfo(core::ServerEnv &env) {
  flat::NodeInfo selfNodeInfo;
  const auto &appInfo = env.appInfo();
  selfNodeInfo.app = appInfo;
  selfNodeInfo.type = flat::NodeType::MGMTD;
  selfNodeInfo.status = flat::NodeStatus::PRIMARY_MGMTD;
  return selfNodeInfo;
}

void recordWriterLatency(std::string_view method, Duration latency) {
  static std::map<String, monitor::LatencyRecorder, std::less<>> latencyRecorders;
  static std::map<String, monitor::CountRecorder, std::less<>> usageRecorders;
  auto lit = latencyRecorders.find(method);
  auto uit = usageRecorders.find(method);
  if (lit == latencyRecorders.end()) {
    monitor::TagSet tags;
    tags.addTag("instance", String(method));
    lit = latencyRecorders.try_emplace(String(method), "MgmtdService.WriterLatency", tags).first;
    uit = usageRecorders.try_emplace(String(method), "MgmtdService.WriterUsage", tags).first;
  }
  lit->second.addSample(latency);
  uit->second.addSample(latency.count());
}
}  // namespace

MgmtdState::MgmtdState(std::shared_ptr<core::ServerEnv> env, const MgmtdConfig &config)
    : env_(std::move(env)),
      config_(config),
      userStore_(*env_->kvEngine(), config_.retry_transaction(), config_.user_cache()) {
  selfNodeInfo_ = initSelfNodeInfo(*env_);
  selfPersistentNodeInfo_ = flat::toPersistentNode(selfNodeInfo_);
}

MgmtdState::~MgmtdState() {}

UtcTime MgmtdState::utcNow() { return env_->utcTimeGenerator()(); }

Result<Void> MgmtdState::validateClusterId(const core::ServiceOperation &ctx, std::string_view clusterId) {
  if (clusterId != env_->appInfo().clusterId) {
    RETURN_AND_LOG_OP_ERR(ctx, MgmtdCode::kClusterIdMismatch, "Cluster id mismatch");
  }
  return Void{};
}

CoTryTask<void> MgmtdState::validateAdmin(const core::ServiceOperation &ctx, const flat::UserInfo &userInfo) {
  if (config_.authenticate()) {
    auto ret = co_await userStore_.getUser(userInfo.token);
    CO_RETURN_ON_ERROR(ret);
    if (!ret->admin) {
      CO_RETURN_AND_LOG_OP_ERR(ctx, MgmtdCode::kNotAdmin, "");
    }
    LOG_OP_INFO(ctx, "Act as admin user {}({})", ret->name, ret->uid.toUnderType());
  }
  co_return Void{};
}

CoTask<std::optional<flat::MgmtdLeaseInfo>> MgmtdState::currentLease(UtcTime now) {
  auto dataPtr = co_await data_.coSharedLock();
  const auto &lease = dataPtr->lease;
  bool canTrustLease =
      lease.lease.has_value() && now + config_.suspicious_lease_interval().asUs() < lease.lease->leaseEnd;
  if (canTrustLease && !lease.bootstrapping) {
    co_return lease.lease;
  }
  co_return std::nullopt;
}

flat::NodeId MgmtdState::selfId() const {
  assert(env_->appInfo().nodeId != 0);
  return flat::NodeId{env_->appInfo().nodeId};
}

MgmtdState::WriterMutexGuard::~WriterMutexGuard() {
  recordWriterLatency(method_, Duration(SteadyClock::now() - start_));
}

kv::FDBRetryStrategy MgmtdState::createRetryStrategy() {
  return kv::FDBRetryStrategy({config_.retry_transaction().max_backoff(),
                               config_.retry_transaction().max_retry_count(),
                               /*retryMaybeCommitted=*/true});
}

}  // namespace hf3fs::mgmtd
