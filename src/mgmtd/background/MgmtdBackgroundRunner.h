#pragma once

#include "common/utils/Coroutine.h"

namespace hf3fs {
class BackgroundRunner;
namespace mgmtd {
class MgmtdHeartbeater;
class MgmtdLeaseExtender;
struct MgmtdState;
class MgmtdChainsUpdater;
class MgmtdClientSessionsChecker;
class MgmtdHeartbeatChecker;
class MgmtdNewBornChainsChecker;
class MgmtdRoutingInfoVersionUpdater;
class MgmtdMetricsUpdater;
class MgmtdTargetInfoPersister;
class MgmtdTargetInfoLoader;
namespace testing {
class MgmtdTestHelper;
}

class MgmtdBackgroundRunner {
 public:
  explicit MgmtdBackgroundRunner(MgmtdState &state);
  ~MgmtdBackgroundRunner();

  void start();
  CoTask<void> stop();

 private:
  MgmtdState &state_;
  std::unique_ptr<BackgroundRunner> backgroundRunner_;
  std::unique_ptr<MgmtdHeartbeater> heartbeater_;
  std::unique_ptr<MgmtdLeaseExtender> leaseExtender_;
  std::unique_ptr<MgmtdChainsUpdater> chainsUpdater_;
  std::unique_ptr<MgmtdClientSessionsChecker> clientSessionsChecker_;
  std::unique_ptr<MgmtdHeartbeatChecker> heartbeatChecker_;
  std::unique_ptr<MgmtdNewBornChainsChecker> newBornChainsChecker_;
  std::unique_ptr<MgmtdRoutingInfoVersionUpdater> routingInfoVersionUpdater_;
  std::unique_ptr<MgmtdMetricsUpdater> metricsUpdater_;
  std::unique_ptr<MgmtdTargetInfoPersister> targetInfoPersister_;
  std::unique_ptr<MgmtdTargetInfoLoader> targetInfoLoader_;

  friend class testing::MgmtdTestHelper;
};
}  // namespace mgmtd
}  // namespace hf3fs
