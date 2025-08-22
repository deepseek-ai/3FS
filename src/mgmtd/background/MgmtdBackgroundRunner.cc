#include "MgmtdBackgroundRunner.h"

#include "MgmtdChainsUpdater.h"
#include "MgmtdClientSessionsChecker.h"
#include "MgmtdHeartbeatChecker.h"
#include "MgmtdHeartbeater.h"
#include "MgmtdLeaseExtender.h"
#include "MgmtdMetricsUpdater.h"
#include "MgmtdNewBornChainsChecker.h"
#include "MgmtdRoutingInfoVersionUpdater.h"
#include "MgmtdTargetInfoLoader.h"
#include "MgmtdTargetInfoPersister.h"
#include "common/utils/BackgroundRunner.h"
#include "mgmtd/service/MgmtdConfig.h"
#include "mgmtd/service/MgmtdState.h"

namespace hf3fs::mgmtd {
MgmtdBackgroundRunner::MgmtdBackgroundRunner(MgmtdState &state)
    : state_(state) {
  if (state_.env_->backgroundExecutor()) {
    backgroundRunner_ = std::make_unique<BackgroundRunner>(*state_.env_->backgroundExecutor());
    heartbeater_ = std::make_unique<MgmtdHeartbeater>(state_);
    leaseExtender_ = std::make_unique<MgmtdLeaseExtender>(state_);
    chainsUpdater_ = std::make_unique<MgmtdChainsUpdater>(state_);
    clientSessionsChecker_ = std::make_unique<MgmtdClientSessionsChecker>(state_);
    heartbeatChecker_ = std::make_unique<MgmtdHeartbeatChecker>(state_);
    newBornChainsChecker_ = std::make_unique<MgmtdNewBornChainsChecker>(state_);
    routingInfoVersionUpdater_ = std::make_unique<MgmtdRoutingInfoVersionUpdater>(state_);
    metricsUpdater_ = std::make_unique<MgmtdMetricsUpdater>(state_);
    targetInfoPersister_ = std::make_unique<MgmtdTargetInfoPersister>(state_);
    targetInfoLoader_ = std::make_unique<MgmtdTargetInfoLoader>(state_);
  }
}

MgmtdBackgroundRunner::~MgmtdBackgroundRunner() {}

void MgmtdBackgroundRunner::start() {
  if (backgroundRunner_) {
    backgroundRunner_->start(
        "extendLease",
        [this] { return leaseExtender_->extend(); },
        state_.config_.extend_lease_interval_getter());
    backgroundRunner_->start(
        "checkClientSessions",
        [this] { return clientSessionsChecker_->check(); },
        state_.config_.check_status_interval_getter());
    backgroundRunner_->start(
        "checkNewBornChains",
        [this] { return newBornChainsChecker_->check(); },
        state_.config_.check_status_interval_getter());
    backgroundRunner_->start(
        "checkHeartbeat",
        [this] { return heartbeatChecker_->check(); },
        state_.config_.check_status_interval_getter());
    backgroundRunner_->start(
        "sendHeartbeat",
        [this] { return heartbeater_->send(); },
        state_.config_.send_heartbeat_interval_getter());
    backgroundRunner_->start(
        "updateChains",
        [this, lastUpdateTs = SteadyClock::now()]() mutable { return chainsUpdater_->update(lastUpdateTs); },
        state_.config_.update_chains_interval_getter());
    backgroundRunner_->start(
        "bumpRoutingInfoVersion",
        [this] { return routingInfoVersionUpdater_->update(); },
        state_.config_.bump_routing_info_version_interval_getter());
    backgroundRunner_->start(
        "updateMetrics",
        [this] { return metricsUpdater_->update(); },
        state_.config_.update_metrics_interval_getter());
    backgroundRunner_->start(
        "persistTargetInfo",
        [this] { return targetInfoPersister_->run(); },
        state_.config_.target_info_persist_interval_getter());
    backgroundRunner_->start(
        "loadTargetInfo",
        [this] { return targetInfoLoader_->run(); },
        state_.config_.target_info_load_interval_getter());
  }
}

CoTask<void> MgmtdBackgroundRunner::stop() {
  if (backgroundRunner_) {
    co_await backgroundRunner_->stopAll();
  }
}
}  // namespace hf3fs::mgmtd
