#pragma once

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/Baton.h>

#include "common/monitor/Recorder.h"
#include "common/utils/BoundedQueue.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"

namespace hf3fs {

class DynamicCoroutinesPool {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_ITEM(queue_size, 1024u);
    CONFIG_HOT_UPDATED_ITEM(threads_num, 8ul, ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(coroutines_num, 64u, ConfigCheckers::checkPositive);
  };

  DynamicCoroutinesPool(const Config &config, std::string_view name = "pool");

  ~DynamicCoroutinesPool() { stopAndJoin(); }

  Result<Void> start();

  Result<Void> stopAndJoin();

  void enqueue(CoTask<void> &&task) { queue_.enqueue(std::make_unique<CoTask<void>>(std::move(task))); }

 protected:
  Result<Void> setCoroutinesNum(uint32_t num);

  CoTask<void> run();

  void afterCoroutineStop();

 private:
  const Config &config_;
  BoundedQueue<std::unique_ptr<CoTask<void>>> queue_;
  std::unique_ptr<ConfigCallbackGuard> guard_;

  std::mutex mutex_;
  folly::CPUThreadPoolExecutor executor_;
  monitor::Recorder::TagRef<monitor::ValueRecorder> coroutinesNumRecorder_;

  std::atomic_flag stopped_{false};
  folly::coro::Baton baton_;
  uint32_t coroutinesNum_{};
  folly::Synchronized<uint32_t, std::mutex> currentRunning_{1};
};

}  // namespace hf3fs
