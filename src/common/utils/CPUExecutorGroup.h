#pragma once

#include <folly/executors/CPUThreadPoolExecutor.h>

#include "common/utils/ConfigBase.h"
#include "common/utils/ConstructLog.h"

namespace hf3fs {
class CPUExecutorGroup {
 public:
  enum class ExecutorStrategy {
    SHARED_QUEUE,  // fallback to CPUThreadPoolExecutor
    SHARED_NOTHING,
    WORK_STEALING,
    ROUND_ROBIN,
    GROUP_WAITING_4,
    GROUP_WAITING_8,
  };

  struct Config : public ConfigBase<Config> {
    CONFIG_ITEM(threadCount, 32);
    CONFIG_ITEM(strategy, ExecutorStrategy::ROUND_ROBIN);
  };

  CPUExecutorGroup(uint32_t threadCount,
                   std::string name,
                   ExecutorStrategy executorStrategy = ExecutorStrategy::SHARED_QUEUE);

  CPUExecutorGroup(std::string name, const Config &config)
      : CPUExecutorGroup(config.threadCount(), std::move(name), config.strategy()) {}

  ~CPUExecutorGroup();

  folly::CPUThreadPoolExecutor &get(size_t i) const { return *executors_[i]; };

  folly::CPUThreadPoolExecutor &randomPick() const;

  folly::CPUThreadPoolExecutor &pickNext() const {
    auto pos = next_.fetch_add(1, std::memory_order_acq_rel);
    return get(pos % size());
  }

  folly::CPUThreadPoolExecutor &pickNextFree() const;

  size_t size() const { return executors_.size(); }

  void join();

  auto &getAll() { return executors_; }
  auto &getAll() const { return executors_; }

  const std::string &getName() const { return name_; }
  folly::ThreadPoolExecutor::PoolStats getPoolStats() const;

 private:
  ConstructLog<"CPUExecutorGroup"> constructLog_;
  const std::string name_;
  std::vector<std::unique_ptr<folly::CPUThreadPoolExecutor>> executors_;
  mutable std::atomic<size_t> next_{0};
};
}  // namespace hf3fs
