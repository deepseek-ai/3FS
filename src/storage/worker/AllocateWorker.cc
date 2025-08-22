#include "storage/worker/AllocateWorker.h"

#include <memory>

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "storage/service/Components.h"

namespace hf3fs::storage {

AllocateWorker::AllocateWorker(const Config &config, Components &components)
    : config_(config),
      components_(components),
      executors_(std::make_pair(1u, 1u), std::make_shared<folly::NamedThreadFactory>("Allocate")) {}

Result<Void> AllocateWorker::start() {
  executors_.add([this] { loop(); });
  started_ = true;
  return Void{};
}

Result<Void> AllocateWorker::stopAndJoin() {
  stopping_ = true;
  cond_.notify_one();
  for (int i = 0; started_ && !stopped_; ++i) {
    XLOGF_IF(INFO, i % 5 == 0, "Waiting for AllocateWorker@{}::loop stop...", fmt::ptr(this));
    std::this_thread::sleep_for(100_ms);
  }
  executors_.join();
  return Void{};
}

void AllocateWorker::loop() {
  while (!stopping_) {
    auto lock = std::unique_lock(mutex_);
    if (cond_.wait_for(lock, 100_ms, [&] { return stopping_.load(); })) {
      break;
    }

    auto minRemainGroups = config_.min_remain_groups();
    auto maxRemainGroups = config_.max_remain_groups();
    auto minRemainUltraGroups = config_.min_remain_ultra_groups();
    auto maxRemainUltraGroups = config_.max_remain_ultra_groups();
    auto maxReserved = config_.max_reserved_chunks();
    for (auto &engine : components_.storageTargets.engines()) {
      engine->allocate_groups(minRemainGroups, maxRemainGroups, 128);
      engine->allocate_ultra_groups(minRemainUltraGroups, maxRemainUltraGroups, 32);
      engine->compact_groups(maxReserved);
    }
  }

  XLOGF(INFO, "AllocateWorker@{}::loop stopped", fmt::ptr(this));
  stopped_ = true;
}

}  // namespace hf3fs::storage
