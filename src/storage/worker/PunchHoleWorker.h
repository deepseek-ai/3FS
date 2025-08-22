#pragma once

#include <atomic>
#include <condition_variable>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <mutex>

#include "storage/service/TargetMap.h"

namespace hf3fs::storage {

struct Components;

class PunchHoleWorker {
 public:
  PunchHoleWorker(Components &components);

  // start recycle worker.
  Result<Void> start();

  // stop recycle worker. End all sync tasks immediately.
  Result<Void> stopAndJoin();

 protected:
  void loop();

 private:
  ConstructLog<"storage::PunchHoleWorker"> constructLog_;
  Components &components_;
  folly::CPUThreadPoolExecutor executors_;

  std::mutex mutex_;
  std::condition_variable cond_;
  std::atomic<bool> stopping_ = false;
  std::atomic<bool> started_ = false;
  std::atomic<bool> stopped_ = false;
};

}  // namespace hf3fs::storage
