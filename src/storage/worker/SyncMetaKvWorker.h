#pragma once

#include <atomic>
#include <condition_variable>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <mutex>

#include "storage/service/TargetMap.h"

namespace hf3fs::storage {

struct Components;

class SyncMetaKvWorker {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(sync_meta_kv_interval, 1_min);
  };

  SyncMetaKvWorker(const Config &config, Components &components);

  Result<Void> start();

  Result<Void> stopAndJoin();

 protected:
  void loop();

  void syncAllMetaKVs();

 private:
  ConstructLog<"storage::SyncMetaKvWorker"> constructLog_;
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
