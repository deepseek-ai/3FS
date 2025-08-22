#include "storage/worker/SyncMetaKvWorker.h"

#include <memory>

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "storage/service/Components.h"

namespace hf3fs::storage {

SyncMetaKvWorker::SyncMetaKvWorker(const Config &config, Components &components)
    : config_(config),
      components_(components),
      executors_(std::make_pair(1u, 1u), std::make_shared<folly::NamedThreadFactory>("SyncMetaKv")) {}

// start recycle worker.
Result<Void> SyncMetaKvWorker::start() {
  executors_.add([this] { loop(); });
  started_ = true;
  return Void{};
}

// stop recycle worker. End all sync tasks immediately.
Result<Void> SyncMetaKvWorker::stopAndJoin() {
  stopping_ = true;
  cond_.notify_one();
  for (int i = 0; started_ && !stopped_; ++i) {
    XLOGF_IF(INFO, i % 5 == 0, "Waiting for SyncMetaKvWorker@{}::loop stop...", fmt::ptr(this));
    std::this_thread::sleep_for(100_ms);
  }
  executors_.join();
  return Void{};
}

void SyncMetaKvWorker::loop() {
  RelativeTime lastSyncMetaKVTime = RelativeTime::now();
  while (!stopping_) {
    auto lock = std::unique_lock(mutex_);
    if (cond_.wait_for(lock, 100_ms, [&] { return stopping_.load(); })) {
      break;
    }

    // 1. sync all meta kvs.
    auto now = RelativeTime::now();
    if (now - lastSyncMetaKVTime >= config_.sync_meta_kv_interval()) {
      lastSyncMetaKVTime = now;
      syncAllMetaKVs();
    }
  }

  XLOGF(INFO, "SyncMetaKvWorker@{}::loop stopped", fmt::ptr(this));
  stopped_ = true;
}

void SyncMetaKvWorker::syncAllMetaKVs() {
  auto snapshot = components_.targetMap.snapshot();
  std::set<uint32_t> offlinedDiskIndex;
  for (auto &[targetId, target] : snapshot->getTargets()) {
    if (target.localState == flat::LocalTargetState::OFFLINE || target.storageTarget == nullptr) {
      continue;
    }
    if (offlinedDiskIndex.contains(target.diskIndex)) {
      continue;
    }

    auto result = target.storageTarget->sync();
    if (UNLIKELY(!result)) {
      XLOGF(CRITICAL, "check disk failed {}, sync error: {}", target.path, result.error());
      components_.targetMap.offlineTargets(target.path.parent_path());
      offlinedDiskIndex.insert(target.diskIndex);
      continue;
    }
  }
}

}  // namespace hf3fs::storage
