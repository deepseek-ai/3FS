#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Sleep.h>
#include <gtest/gtest.h>
#include <thread>

#include "common/utils/Coroutine.h"
#include "common/utils/Semaphore.h"
#include "common/utils/SemaphoreGuard.h"

namespace hf3fs::test {
namespace {

CoTask<size_t> runTask(Semaphore &sema, size_t numLoops) {
  for (size_t i = 0; i < numLoops; i++) {
    SemaphoreGuard lock(sema);
    co_await lock.coWait();
    co_await folly::coro::sleep(std::chrono::microseconds{0});
  }

  co_return numLoops;
}

std::vector<folly::SemiFuture<size_t>> startTasks(Semaphore &sema,
                                                  folly::CPUThreadPoolExecutor &threadPool,
                                                  size_t numTasks,
                                                  size_t numLoops) {
  std::vector<folly::SemiFuture<size_t>> tasks;

  for (size_t t = 0; t < numTasks; t++) {
    tasks.push_back(runTask(sema, numLoops).scheduleOn(folly::Executor::getKeepAliveToken(threadPool)).start());
  }

  return tasks;
}

TEST(TestSemaphore, Basic) {
  size_t numCpuCores = std::thread::hardware_concurrency();
  size_t numTasks = numCpuCores * 2;
  size_t numLoops = 100;

  Semaphore sema(std::max(size_t(1), numCpuCores / 2));
  folly::CPUThreadPoolExecutor threadPool(numCpuCores / 2);

  std::vector<folly::SemiFuture<size_t>> tasks = startTasks(sema, threadPool, numTasks, numLoops);
  auto results = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(tasks)));

  size_t sumLoops = 0;
  for (auto res : results) sumLoops += res;
  ASSERT_EQ(numTasks * numLoops, sumLoops);
}

TEST(TestSemaphore, ChangeUsableTokens) {
  size_t numCpuCores = std::thread::hardware_concurrency();
  size_t numTasks = numCpuCores * 2;
  size_t numLoops = 100;

  Semaphore sema(std::max(size_t(1), numCpuCores / 2));
  folly::CPUThreadPoolExecutor threadPool(numCpuCores / 2);

  std::vector<folly::SemiFuture<size_t>> tasks = startTasks(sema, threadPool, numTasks, numLoops);
  sema.changeUsableTokens(numTasks);
  auto results = folly::coro::blockingWait(folly::coro::collectAllRange(std::move(tasks)));

  size_t sumLoops = 0;
  for (auto res : results) sumLoops += res;
  ASSERT_EQ(numTasks * numLoops, sumLoops);
}

}  // namespace
}  // namespace hf3fs::test
