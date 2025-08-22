#pragma once

#include <atomic>
#include <condition_variable>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <mutex>

#include "storage/service/TargetMap.h"

namespace hf3fs::storage {

struct Components;

class AllocateWorker {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(min_remain_groups, 4ul);
    CONFIG_HOT_UPDATED_ITEM(max_remain_groups, 8ul);
    CONFIG_HOT_UPDATED_ITEM(min_remain_ultra_groups, 0ul);  // greater than 4MiB
    CONFIG_HOT_UPDATED_ITEM(max_remain_ultra_groups, 4ul);
    CONFIG_HOT_UPDATED_ITEM(max_reserved_chunks, 1_GB);
  };

  AllocateWorker(const Config &config, Components &components);

  Result<Void> start();

  Result<Void> stopAndJoin();

 protected:
  void loop();

 private:
  ConstructLog<"storage::AllocateWorker"> constructLog_;
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
