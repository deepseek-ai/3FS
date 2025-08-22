#pragma once

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/executors/TimekeeperScheduledExecutor.h>
#include <folly/experimental/coro/WithCancellation.h>
#include <folly/experimental/symbolizer/Symbolizer.h>
#include <folly/synchronization/Baton.h>

#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/ExecutorStatsReporter.h"

namespace hf3fs::net {

class ThreadPoolGroup {
 public:
  struct Config : public ConfigBase<Config> {
    CONFIG_ITEM(num_proc_threads, 2ul);
    CONFIG_ITEM(num_io_threads, 2ul);
    CONFIG_ITEM(num_bg_threads, 2ul);
    CONFIG_ITEM(num_connect_threads, 2ul);
    CONFIG_ITEM(collect_stats, false);
    CONFIG_ITEM(enable_work_stealing, false);  // deprecated
    CONFIG_ITEM(proc_thread_pool_stratetry, CPUExecutorGroup::ExecutorStrategy::SHARED_QUEUE);
    CONFIG_ITEM(io_thread_pool_stratetry, CPUExecutorGroup::ExecutorStrategy::SHARED_QUEUE);
    CONFIG_ITEM(bg_thread_pool_stratetry, CPUExecutorGroup::ExecutorStrategy::SHARED_QUEUE);
  };

  ThreadPoolGroup(const std::string &name, const Config &config)
      : fullname_(fmt::format("{}@{}", name, fmt::ptr(&config))),
        startStacktrace_(folly::symbolizer::getStackTraceStr()),
        procThreadPool_(config.num_proc_threads(), name + "Proc", config.proc_thread_pool_stratetry()),
        ioThreadPool_(config.num_io_threads(), name + "IO", config.io_thread_pool_stratetry()),
        bgThreadPool_(config.num_bg_threads(), name + "BG", config.bg_thread_pool_stratetry()),
        connThreadPool_(config.num_connect_threads(),
                        config.num_connect_threads(),
                        std::make_shared<folly::NamedThreadFactory>(name + "Conn")),
        procThreadPoolReporter_(procThreadPool_),
        ioThreadPoolReporter_(ioThreadPool_),
        bgThreadPoolReporter_(bgThreadPool_),
        connThreadPoolReporter_(connThreadPool_),
        stopping_(false) {
    if (config.collect_stats()) {
      future_ = co_withCancellation(cancel_.getToken(), collectStatsPeriodically())
                    .scheduleOn(&bgThreadPool_.randomPick())
                    .start();
    }
  }

  ~ThreadPoolGroup() {
    if (!stopping_) {
      XLOGF(WARN, "Thread pool group '{}' not stopped before destructed, created by {}", fullname_, startStacktrace_);
      stopAndJoin();
    }
  }

  auto &procThreadPool() { return procThreadPool_; }
  auto &ioThreadPool() { return ioThreadPool_; }
  auto &bgThreadPool() { return bgThreadPool_; }
  auto &connThreadPool() { return connThreadPool_; }

  void stopAndJoin();

 private:
  CoTask<void> collectStatsPeriodically();

 private:
  ConstructLog<"net::ThreadPoolGroup"> constructLog_;
  std::string fullname_;
  std::string startStacktrace_;
  CPUExecutorGroup procThreadPool_;
  CPUExecutorGroup ioThreadPool_;
  CPUExecutorGroup bgThreadPool_;
  folly::IOThreadPoolExecutor connThreadPool_;
  ExecutorStatsReporter<CPUExecutorGroup> procThreadPoolReporter_;
  ExecutorStatsReporter<CPUExecutorGroup> ioThreadPoolReporter_;
  ExecutorStatsReporter<CPUExecutorGroup> bgThreadPoolReporter_;
  ExecutorStatsReporter<folly::IOThreadPoolExecutor> connThreadPoolReporter_;
  folly::CancellationSource cancel_;
  folly::SemiFuture<Void> future_;
  std::atomic_bool stopping_;
};

}  // namespace hf3fs::net
