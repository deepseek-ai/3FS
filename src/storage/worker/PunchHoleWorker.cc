#include "storage/worker/PunchHoleWorker.h"

#include <memory>

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "storage/service/Components.h"

namespace hf3fs::storage {

PunchHoleWorker::PunchHoleWorker(Components &components)
    : components_(components),
      executors_(std::make_pair(1u, 1u), std::make_shared<folly::NamedThreadFactory>("PunchHole")) {}

// start recycle worker.
Result<Void> PunchHoleWorker::start() {
  executors_.add([this] { loop(); });
  started_ = true;
  return Void{};
}

// stop recycle worker. End all sync tasks immediately.
Result<Void> PunchHoleWorker::stopAndJoin() {
  stopping_ = true;
  cond_.notify_one();
  for (int i = 0; started_ && !stopped_; ++i) {
    XLOGF_IF(INFO, i % 5 == 0, "Waiting for PunchHoleWorker@{}::loop stop...", fmt::ptr(this));
    std::this_thread::sleep_for(100_ms);
  }
  executors_.join();
  return Void{};
}

void PunchHoleWorker::loop() {
  RelativeTime lastPunchHoleTime = RelativeTime::now();

  bool allTargetsRecycled = true;
  while (!stopping_) {
    if (allTargetsRecycled) {
      auto lock = std::unique_lock(mutex_);
      if (cond_.wait_for(lock, 1_s, [&] { return stopping_.load(); })) {
        break;
      }
    }

    // 1. recycle all targets.
    if (RelativeTime::now() - lastPunchHoleTime >= 10_s) {
      std::vector<std::weak_ptr<StorageTarget>> targets;
      {
        auto targetMap = components_.targetMap.snapshot();
        for (auto &[targetId, target] : targetMap->getTargets()) {
          if (target.unrecoverableOffline()) {
            continue;
          }
          if (target.localState != flat::LocalTargetState::OFFLINE && target.storageTarget != nullptr) {
            targets.push_back(target.storageTarget);
          }
        }
      }

      allTargetsRecycled = true;
      for (auto &weakTarget : targets) {
        if (stopping_) {
          break;
        }
        auto target = weakTarget.lock();
        if (!target) {
          continue;
        }

        bool targetRecycled = false;
        for (auto i = 0u; i < 128u && !stopping_; ++i) {
          auto result = target->punchHole();
          if (result.hasError()) {
            XLOGF(ERR, "recycle target {} failed: {}", target->path(), result.error());
            targetRecycled = true;
            break;
          } else if (*result) {
            targetRecycled = true;
            break;
          }
        }
        allTargetsRecycled &= targetRecycled;
      }

      lastPunchHoleTime = RelativeTime::now();
    }
  }
  stopped_ = true;
  XLOGF(INFO, "PunchHoleWorker@{}::loop stopped", fmt::ptr(this));
}

}  // namespace hf3fs::storage
