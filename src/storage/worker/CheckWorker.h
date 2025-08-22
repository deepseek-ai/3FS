#pragma once

#include <atomic>
#include <condition_variable>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <mutex>

#include "common/utils/ConfigBase.h"
#include "storage/service/TargetMap.h"

namespace hf3fs::storage {

struct Components;

class CheckWorker {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(update_target_size_interval, 10_s);
    CONFIG_HOT_UPDATED_ITEM(emergency_recycling_ratio, 0.95);
    CONFIG_HOT_UPDATED_ITEM(disk_low_space_threshold, 0.96);
    CONFIG_HOT_UPDATED_ITEM(disk_reject_create_chunk_threshold, 0.98);
  };

  CheckWorker(const Config &config, Components &components);

  // start check worker.
  Result<Void> start(const std::vector<Path> &targetPaths, const std::vector<std::string> &manufacturers);

  // stop check worker. End all tasks immediately.
  Result<Void> stopAndJoin();

  // check hf3fs path writable or not.
  static bool checkWritable(const Path &path);

 protected:
  void loop(const std::vector<Path> &targetPaths, const std::vector<std::string> &manufacturers);

 private:
  ConstructLog<"storage::CheckWorker"> constructLog_;
  const Config &config_;
  Components &components_;
  folly::CPUThreadPoolExecutor executors_;

  std::mutex mutex_;
  std::condition_variable cond_;
  std::atomic<bool> stopping_ = false;
  std::atomic<bool> started_ = false;
  std::atomic<bool> stopped_ = false;
};

}  // namespace hf3fs::storage
