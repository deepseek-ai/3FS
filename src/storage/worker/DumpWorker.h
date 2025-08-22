#pragma once

#include <atomic>
#include <condition_variable>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <mutex>

#include "fbs/storage/Common.h"
#include "storage/service/TargetMap.h"

namespace hf3fs::storage {

struct Components;

class DumpWorker {
 public:
  struct Config : ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(dump_root_path, Path{});
    CONFIG_HOT_UPDATED_ITEM(dump_interval, 1_d);
    CONFIG_HOT_UPDATED_ITEM(high_cpu_usage_threshold, 100);  // num of cores.
  };

  DumpWorker(const Config &config, Components &components);

  // start dump worker.
  Result<Void> start(flat::NodeId nodeId);

  // stop dump worker. End all tasks immediately.
  Result<Void> stopAndJoin();

 protected:
  void loop();

  Result<Void> dump(const Path &rootPath);

  Result<Void> profilerStart(const Path &rootPath);

 private:
  ConstructLog<"storage::DumpWorker"> constructLog_;
  const Config &config_;
  Components &components_;
  folly::CPUThreadPoolExecutor executors_;

  std::mutex mutex_;
  std::condition_variable cond_;
  std::atomic<bool> stopping_ = false;
  std::atomic<bool> started_ = false;
  std::atomic<bool> stopped_ = false;
  flat::NodeId nodeId_{};
};

}  // namespace hf3fs::storage
