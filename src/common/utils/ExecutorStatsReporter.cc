#include "common/utils/ExecutorStatsReporter.h"

#include <folly/executors/IOThreadPoolExecutor.h>

#include "common/utils/CPUExecutorGroup.h"

namespace hf3fs {
namespace {

monitor::CountRecorder threadCountRecorder{"thread_pool_group.thread_count"};
monitor::CountRecorder idleThreadCountRecorder{"thread_pool_group.idle_thread_count"};
monitor::CountRecorder activeThreadCountRecorder{"thread_pool_group.active_thread_count"};
monitor::CountRecorder pendingTaskCountRecorder{"thread_pool_group.pending_task_count"};
monitor::CountRecorder totalTaskCountRecorder{"thread_pool_group.total_task_count"};

}  // namespace

template <class T>
ExecutorStatsReporter<T>::ExecutorStatsReporter(const T &executor)
    : executor_(executor),
      threadCountRecorder_(threadCountRecorder.getRecorderWithTag(monitor::threadTagSet(executor.getName()))),
      idleThreadCountRecorder_(idleThreadCountRecorder.getRecorderWithTag(monitor::threadTagSet(executor.getName()))),
      activeThreadCountRecorder_(
          activeThreadCountRecorder.getRecorderWithTag(monitor::threadTagSet(executor.getName()))),
      pendingTaskCountRecorder_(pendingTaskCountRecorder.getRecorderWithTag(monitor::threadTagSet(executor.getName()))),
      totalTaskCountRecorder_(totalTaskCountRecorder.getRecorderWithTag(monitor::threadTagSet(executor.getName()))) {}

template <class T>
void ExecutorStatsReporter<T>::report() {
  auto stats = executor_.getPoolStats();
  threadCountRecorder_->addSample(stats.threadCount);
  idleThreadCountRecorder_->addSample(stats.idleThreadCount);
  activeThreadCountRecorder_->addSample(stats.activeThreadCount);
  pendingTaskCountRecorder_->addSample(stats.pendingTaskCount);
  totalTaskCountRecorder_->addSample(stats.totalTaskCount);
}

template class ExecutorStatsReporter<CPUExecutorGroup>;
template class ExecutorStatsReporter<folly::CPUThreadPoolExecutor>;
template class ExecutorStatsReporter<folly::IOThreadPoolExecutor>;

}  // namespace hf3fs
