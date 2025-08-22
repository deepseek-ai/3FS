#pragma once

#include <folly/Executor.h>
#include <variant>

#include "CPUExecutorGroup.h"
#include "Coroutine.h"
#include "CountDownLatch.h"
#include "Duration.h"

namespace hf3fs {
class BackgroundRunner {
 public:
  explicit BackgroundRunner(folly::Executor::KeepAlive<> executor);
  explicit BackgroundRunner(CPUExecutorGroup &executor);

  ~BackgroundRunner();

  using TaskGetter = std::function<CoTask<void>()>;
  using IntervalGetter = std::function<Duration()>;
  bool start(String taskName, TaskGetter taskGetter, IntervalGetter intervalGetter);
  bool startOnce(String taskName, TaskGetter taskGetter);
  CoTask<void> stopAll();

 private:
  folly::Executor::KeepAlive<> getExecutor();
  CoTask<void> run(String taskName, TaskGetter taskGetter, IntervalGetter intervalGetter);

  bool stopping_ = false;
  std::variant<folly::Executor::KeepAlive<>, CPUExecutorGroup *> executor_;
  CancellationSource cancel_;
  CountDownLatch<> latch_;
  std::mutex mutex_;
};
}  // namespace hf3fs
