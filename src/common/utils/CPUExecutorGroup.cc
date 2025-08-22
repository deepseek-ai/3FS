#include "CPUExecutorGroup.h"

#include <folly/executors/task_queue/UnboundedBlockingQueue.h>
#include <folly/logging/xlog.h>

#include "common/utils/SimpleSemaphore.h"
#include "common/utils/StringUtils.h"
#include "common/utils/WorkStealingBlockingQueue.h"

namespace hf3fs {
namespace {

using Task = folly::CPUThreadPoolExecutor::CPUTask;

auto isEndTask = [](const Task &task) { return task.poison; };
using EndTaskPredicate = std::decay_t<decltype(isEndTask)>;

template <template <typename, typename> typename QueueTemplate>
std::vector<std::unique_ptr<folly::CPUThreadPoolExecutor>> createExecutors(const String &name, size_t count) {
  std::vector<std::unique_ptr<folly::CPUThreadPoolExecutor>> executors;
  executors.reserve(count);

  using Queue = QueueTemplate<Task, EndTaskPredicate>;

  auto threadFactory = std::make_shared<folly::NamedThreadFactory>(name);
  auto sharedState = std::make_shared<typename Queue::SharedState>(count);
  auto pair = std::make_pair(1U, 1U);

  for (size_t i = 0; i < count; ++i) {
    auto queue = std::make_unique<Queue>(sharedState, i, isEndTask);
    auto executor = std::make_unique<folly::CPUThreadPoolExecutor>(pair, std::move(queue), threadFactory);
    executors.push_back(std::move(executor));
  }
  return executors;
};

std::vector<std::unique_ptr<folly::CPUThreadPoolExecutor>> createGroupWaitingExecutors(const String &name,
                                                                                       size_t threadCount,
                                                                                       size_t groupSize) {
  auto threadFactory = std::make_shared<folly::NamedThreadFactory>(name);
  std::vector<std::unique_ptr<folly::CPUThreadPoolExecutor>> executors;
  if (threadCount % groupSize != 0) {
    throw StatusException(Status(
        StatusCode::kInvalidArg,
        fmt::format("GROUP_WAITING_{} only supports threadCount times of {}: {}", groupSize, threadCount, groupSize)));
  }
  for (size_t i = 0; i < threadCount / groupSize; ++i) {
    executors.push_back(std::make_unique<folly::CPUThreadPoolExecutor>(
        std::make_pair(groupSize, groupSize),
        std::make_unique<folly::UnboundedBlockingQueue<Task, SimpleSemaphore>>(),
        threadFactory));
  }
  return executors;
}
}  // namespace

CPUExecutorGroup::CPUExecutorGroup(uint32_t threadCount, std::string name, ExecutorStrategy executorStrategy)
    : name_(std::move(name)) {
  auto threadFactory = std::make_shared<folly::NamedThreadFactory>(name_);
  using enum ExecutorStrategy;
  switch (executorStrategy) {
    case SHARED_QUEUE:
      executors_.push_back(
          std::make_unique<folly::CPUThreadPoolExecutor>(std::make_pair(threadCount, threadCount), threadFactory));
      break;
    case SHARED_NOTHING:
      executors_ = createExecutors<SharedNothingBlockingQueue>(name_, threadCount);
      break;
    case WORK_STEALING:
      executors_ = createExecutors<WorkStealingBlockingQueue>(name_, threadCount);
      break;
    case ROUND_ROBIN:
      executors_ = createExecutors<RoundRobinBlockingQueue>(name_, threadCount);
      break;
    case GROUP_WAITING_4:
      executors_ = createGroupWaitingExecutors(name_, threadCount, 4);
      break;
    case GROUP_WAITING_8:
      executors_ = createGroupWaitingExecutors(name_, threadCount, 8);
      break;
  }
}

CPUExecutorGroup::~CPUExecutorGroup() { join(); }

folly::CPUThreadPoolExecutor &CPUExecutorGroup::randomPick() const {
  auto pos = folly::Random::rand32(size());
  return get(pos);
};

folly::CPUThreadPoolExecutor &CPUExecutorGroup::pickNextFree() const {
  if (size() == 1) {
    return get(0);
  }
  auto start = next_.fetch_add(1, std::memory_order_acq_rel);
  auto probeSize = std::min(size(), 4UL);
  auto minPos = size() + 1;
  auto minSize = std::numeric_limits<size_t>::max();
  for (size_t i = 0; i < probeSize; ++i) {
    auto pos = (start + i) % size();
    auto queueSize = get(pos).getTaskQueueSize();
    if (queueSize < minSize) {
      minSize = queueSize;
      minPos = pos;
    }
  }
  return get(minPos);
}

void CPUExecutorGroup::join() {
  XLOGF(DBG, "CPUExecutorGroup {} start join", fmt::ptr(this));

  for (auto &executor : executors_) {
    XLOGF(DBG, "CPUExecutorGroup {} join executor {} ...", fmt::ptr(this), fmt::ptr(executor.get()));
    executor->join();
    XLOGF(DBG, "CPUExecutorGroup {} join executor {} done", fmt::ptr(this), fmt::ptr(executor.get()));
  }
  XLOGF(DBG, "CPUExecutorGroup {} join done", fmt::ptr(this));
}

folly::ThreadPoolExecutor::PoolStats CPUExecutorGroup::getPoolStats() const {
  auto total = folly::ThreadPoolExecutor::PoolStats{};
  for (const auto &e : getAll()) {
    auto stats = e->getPoolStats();
    total.threadCount += stats.threadCount;
    total.idleThreadCount += stats.idleThreadCount;
    total.activeThreadCount += stats.activeThreadCount;
    total.pendingTaskCount += stats.pendingTaskCount;
    total.totalTaskCount += stats.totalTaskCount;
    total.maxIdleTime = std::max(total.maxIdleTime, stats.maxIdleTime);
  }
  return total;
}
}  // namespace hf3fs
