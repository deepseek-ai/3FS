#include "storage/worker/CheckWorker.h"

#include <fstream>

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "common/utils/UtcTime.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "storage/service/Components.h"

namespace hf3fs::storage {
namespace {

monitor::ValueRecorder new_chunk_engine_count = monitor::ValueRecorder{"storage.chunk_engine.new", std::nullopt, false};
monitor::ValueRecorder old_chunk_engine_count = monitor::ValueRecorder{"storage.chunk_engine.old", std::nullopt, false};

struct Recorders {
  monitor::ValueRecorder disk_capacity;
  monitor::ValueRecorder disk_readonly;
  monitor::ValueRecorder disk_available;
  monitor::ValueRecorder disk_free;
  monitor::OperationRecorder check_disk;
  monitor::ValueRecorder position_count;
  monitor::ValueRecorder position_rc;
  monitor::CountRecorder copy_on_write_times;
  monitor::LatencyRecorder copy_on_write_latency;
  monitor::CountRecorder copy_on_write_read_times;
  monitor::CountRecorder copy_on_write_read_bytes;
  monitor::LatencyRecorder copy_on_write_read_latency;
  monitor::CountRecorder checksum_reuse;
  monitor::CountRecorder checksum_combine;
  monitor::CountRecorder checksum_recalculate;
  monitor::CountRecorder safe_write_direct_append;
  monitor::CountRecorder safe_write_indirect_append;
  monitor::CountRecorder safe_write_truncate_shorten;
  monitor::CountRecorder safe_write_truncate_extend;
  monitor::CountRecorder safe_write_read_tail_times;
  monitor::CountRecorder safe_write_read_tail_bytes;
  monitor::CountRecorder allocate_times;
  monitor::LatencyRecorder allocate_latency;
  monitor::CountRecorder pwrite_times;
  monitor::LatencyRecorder pwrite_latency;

  Recorders(const monitor::TagSet &tag)
      : disk_capacity("storage.disk_info.capacity", tag, false),
        disk_readonly("storage.disk_info.read_only", tag, false),
        disk_available("storage.disk_info.available", tag, false),
        disk_free("storage.disk_info.free", tag, false),
        check_disk("storage.check_disk", tag),
        position_count("storage.chunk_engine.position_count", tag, false),
        position_rc("storage.chunk_engine.position_rc", tag, false),
        copy_on_write_times("storage.chunk_engine.copy_on_write_times", tag),
        copy_on_write_latency("storage.chunk_engine.copy_on_write_latency", tag),
        copy_on_write_read_times("storage.chunk_engine.copy_on_write_read_times", tag),
        copy_on_write_read_bytes("storage.chunk_engine.copy_on_write_read_bytes", tag),
        copy_on_write_read_latency("storage.chunk_engine.copy_on_write_read_latency", tag),
        checksum_reuse("storage.chunk_engine.checksum_reuse", tag),
        checksum_combine("storage.chunk_engine.checksum_combine", tag),
        checksum_recalculate("storage.chunk_engine.checksum_recalculate", tag),
        safe_write_direct_append("storage.chunk_engine.safe_write_direct_append", tag),
        safe_write_indirect_append("storage.chunk_engine.safe_write_indirect_append", tag),
        safe_write_truncate_shorten("storage.chunk_engine.safe_write_truncate_shorten", tag),
        safe_write_truncate_extend("storage.chunk_engine.safe_write_truncate_extend", tag),
        safe_write_read_tail_times("storage.chunk_engine.safe_write_read_tail_times", tag),
        safe_write_read_tail_bytes("storage.chunk_engine.safe_write_read_tail_bytes", tag),
        allocate_times("storage.chunk_engine.allocate_times", tag),
        allocate_latency("storage.chunk_engine.allocate_latency", tag),
        pwrite_times("storage.chunk_engine.pwrite_times", tag),
        pwrite_latency("storage.chunk_engine.pwrite_latency", tag) {}
};

}  // namespace

CheckWorker::CheckWorker(const Config &config, Components &components)
    : config_(config),
      components_(components),
      executors_(std::make_pair(1u, 1u), std::make_shared<folly::NamedThreadFactory>("Check")) {}

// start check worker.
Result<Void> CheckWorker::start(const std::vector<Path> &targetPaths, const std::vector<std::string> &manufacturers) {
  executors_.add([this, targetPaths, manufacturers] { loop(targetPaths, manufacturers); });
  started_ = true;
  return Void{};
}

// stop check worker. End all tasks immediately.
Result<Void> CheckWorker::stopAndJoin() {
  stopping_ = true;
  cond_.notify_one();
  for (int i = 0; started_ && !stopped_; ++i) {
    XLOGF_IF(INFO, i % 5 == 0, "Waiting for CheckWorker@{}::loop stop...", fmt::ptr(this));
    std::this_thread::sleep_for(100_ms);
  }
  executors_.join();
  return Void{};
}

void CheckWorker::loop(const std::vector<Path> &targetPaths, const std::vector<std::string> &manufacturers) {
  (void)manufacturers;

  // 0. initialize records.
  static auto recorders = [&] {
    std::vector<std::unique_ptr<Recorders>> recorders;
    for (auto i = 0ul; i < targetPaths.size(); ++i) {
      monitor::TagSet tag;
      tag.addTag("instance", std::to_string(i));
      recorders.push_back(std::make_unique<Recorders>(tag));
    }
    return recorders;
  }();

  RelativeTime lastCheckDiskStatusTime{};
  RelativeTime lastCleanUpExpiredClientsTime{};
  RelativeTime lastTriggerHeartbeatTime{};
  RelativeTime lastUpdateTargetUsedSizeTime = RelativeTime::now();
  RelativeTime lastChunkEngineMetricsReportTime = RelativeTime::now();
  robin_hood::unordered_map<uint32_t, double> diskUsage;
  while (!stopping_) {
    auto lock = std::unique_lock(mutex_);
    if (cond_.wait_for(lock, 100_ms, [&] { return stopping_.load(); })) {
      break;
    }

    // 1. reload offline targets.
    {
      auto snapshot = components_.targetMap.snapshot();
      for (auto &[targetId, target] : snapshot->getTargets()) {
        if (target.unrecoverableOffline()) {
          continue;
        }
        if (target.localState == flat::LocalTargetState::OFFLINE) {
          if (target.weakStorageTarget.expired()) {
            auto result = components_.storageTargets.loadTarget(target.path);
            if (UNLIKELY(!result)) {
              XLOGF(ERR, "CheckWorker@{} reload target {} failed", fmt::ptr(this), target.path);
            } else {
              XLOGF(INFO, "CheckWorker@{} reload target {} succ", fmt::ptr(this), target.path);
              components_.refreshRoutingInfo();
            }
          } else {
            XLOGF(WARNING, "CheckWorker@{} offline target {} is still being used", fmt::ptr(this), target.path);
          }
        }
      }
    }

    // 2. check disk status.
    auto now = RelativeTime::now();
    auto diskLowSpaceThreshold = config_.disk_low_space_threshold();
    auto diskRejectCreateChunkThreshold = config_.disk_reject_create_chunk_threshold();
    if (now - lastCheckDiskStatusTime >= 3_s) {
      lastCheckDiskStatusTime = now;
      XLOGF(DBG9, "check disk status start");
      for (auto i = 0ul; i < targetPaths.size(); ++i) {
        auto &targetPath = targetPaths[i];
        auto &recorder = *recorders[i];
        boost::system::error_code ec{};
        auto spaceInfo = boost::filesystem::space(targetPath, ec);
        if (UNLIKELY(ec.failed())) {
          XLOGF(CRITICAL, "check disk failed {}, errno: {}", targetPath, ec.message());
          components_.targetMap.offlineTargets(targetPath);
          continue;
        }

        recorder.disk_capacity.set(spaceInfo.capacity);
        recorder.disk_free.set(spaceInfo.available);
        diskUsage[i] = 1.0 - (double)spaceInfo.available / std::max(1ul, spaceInfo.capacity);

        auto recordGuard = recorder.check_disk.record();
        bool writable = checkWritable(targetPath);
        if (!writable) {
          recorder.disk_readonly.set(1);
          XLOGF(CRITICAL, "check disk failed {}, readonly", targetPath);
          components_.targetMap.offlineTargets(targetPath);
          continue;
        }
        recordGuard.report(true);

        bool lowSpace = diskUsage[i] >= diskLowSpaceThreshold;
        bool rejectCreateChunk = diskUsage[i] >= diskRejectCreateChunkThreshold;
        components_.storageTargets.engines()[i]->set_allow_to_allocate(!rejectCreateChunk);
        components_.targetMap.updateDiskState(targetPath, lowSpace, rejectCreateChunk);
      }
      XLOGF(DBG9, "check disk status finished");
    }

    // 3. clean up expired clients.
    now = RelativeTime::now();
    if (now - lastCleanUpExpiredClientsTime >= 60_s) {
      lastCleanUpExpiredClientsTime = now;
      auto result = components_.getActiveClientsList();
      if (result) {
        components_.reliableUpdate.cleanUpExpiredClients(*result);
      } else if (result.error().code() != StorageClientCode::kRoutingError) {
        XLOGF(ERR, "get active clients list error: {}", result.error());
      }
    }

    // 4. update target used size.
    now = RelativeTime::now();
    auto emergencyRecyclingRatio = config_.emergency_recycling_ratio();
    if (now - lastUpdateTargetUsedSizeTime >= config_.update_target_size_interval()) {
      lastUpdateTargetUsedSizeTime = now;
      components_.targetMap.updateTargetUsedSize();

      robin_hood::unordered_map<uint32_t, uint64_t> diskUnusedSize;
      robin_hood::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> chunkEngineCount;
      auto snapshot = components_.targetMap.snapshot();
      for (auto &[targetId, target] : snapshot->getTargets()) {
        if (!target.unrecoverableOffline() && target.localState != flat::LocalTargetState::OFFLINE &&
            target.storageTarget != nullptr) {
          target.storageTarget->reportUnrecycledSize();
          target.storageTarget->setEmergencyRecycling(diskUsage[target.diskIndex] >= emergencyRecyclingRatio);
          diskUnusedSize[target.diskIndex] += target.storageTarget->unusedSize();
          if (target.storageTarget->useChunkEngine()) {
            chunkEngineCount[target.diskIndex].first++;
          } else {
            chunkEngineCount[target.diskIndex].second++;
          }
        }
      }
      for (auto i = 0ul; i < targetPaths.size(); ++i) {
        auto tag = monitor::instanceTagSet(std::to_string(i));
        auto [new_count, old_count] = chunkEngineCount[i];
        new_chunk_engine_count.set(new_count, tag);
        old_chunk_engine_count.set(old_count, tag);
        auto rawUsedSize = components_.storageTargets.engines()[i]->raw_used_size();
        auto &recorder = *recorders[i];
        recorder.disk_available.set(diskUnusedSize[i] + rawUsedSize.reserved_size + recorder.disk_free.value());
        recorder.position_count.set(rawUsedSize.position_count);
        recorder.position_rc.set(rawUsedSize.position_rc);
      }
    }

    // 5. trigger heartbeat if need.
    if (now - lastTriggerHeartbeatTime >= 1_s) {
      lastTriggerHeartbeatTime = now;
      components_.triggerHeartbeatIfNeed();
    }

    // 6. report chunk engine metrics.
    now = RelativeTime::now();
    if (now - lastChunkEngineMetricsReportTime >= 1_s) {
      lastChunkEngineMetricsReportTime = now;
      for (auto i = 0ul; i < targetPaths.size(); ++i) {
        auto &recorder = *recorders[i];
        auto metrics = components_.storageTargets.engines()[i]->get_metrics();
        recorder.copy_on_write_times.addSample(metrics.copy_on_write_times);
        if (metrics.copy_on_write_latency) {
          recorder.copy_on_write_latency.addSample(std::chrono::microseconds(metrics.copy_on_write_latency));
        }
        recorder.copy_on_write_read_times.addSample(metrics.copy_on_write_read_times);
        recorder.copy_on_write_read_bytes.addSample(metrics.copy_on_write_read_bytes);
        if (metrics.copy_on_write_read_latency) {
          recorder.copy_on_write_read_latency.addSample(std::chrono::microseconds(metrics.copy_on_write_read_latency));
        }
        recorder.checksum_reuse.addSample(metrics.checksum_reuse);
        recorder.checksum_combine.addSample(metrics.checksum_combine);
        recorder.checksum_recalculate.addSample(metrics.checksum_recalculate);
        recorder.safe_write_direct_append.addSample(metrics.safe_write_direct_append);
        recorder.safe_write_indirect_append.addSample(metrics.safe_write_indirect_append);
        recorder.safe_write_truncate_shorten.addSample(metrics.safe_write_truncate_shorten);
        recorder.safe_write_truncate_extend.addSample(metrics.safe_write_truncate_extend);
        recorder.safe_write_read_tail_times.addSample(metrics.safe_write_read_tail_times);
        recorder.safe_write_read_tail_bytes.addSample(metrics.safe_write_read_tail_bytes);
        recorder.allocate_times.addSample(metrics.allocate_times);
        if (metrics.allocate_latency) {
          recorder.allocate_latency.addSample(std::chrono::microseconds(metrics.allocate_latency));
        }
        recorder.pwrite_times.addSample(metrics.pwrite_times);
        if (metrics.pwrite_latency) {
          recorder.pwrite_latency.addSample(std::chrono::microseconds(metrics.pwrite_latency));
        }
      }
    }
  }
  stopped_ = true;
  XLOGF(INFO, "CheckWorker@{}::loop stopped", fmt::ptr(this));
}

bool CheckWorker::checkWritable(const Path &path) {
  std::ofstream check(path / ".hf3fs_check", std::ios::out);
  return check && (check << fmt::format("{}", UtcTime{UtcClock::now()}));
}

}  // namespace hf3fs::storage
