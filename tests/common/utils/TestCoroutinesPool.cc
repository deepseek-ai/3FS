#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>

#include "common/utils/CoroutinesPool.h"
#include "common/utils/CountDownLatch.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

void testCoroutinesPool(bool enableWorkStealing) {
  struct Job {
    int cnt = 0;
  };

  folly::CPUThreadPoolExecutor executor(4);

  CoroutinesPool<Job>::Config config;
  config.set_enable_work_stealing(enableWorkStealing);
  CoroutinesPool<Job> pool(config, &executor);

  SCOPE_EXIT { pool.stopAndJoin(); };

  int n = 100;
  CountDownLatch latch(2 * n);

  std::atomic<int> sum = 0;
  pool.start([&sum, &latch](Job job) -> CoTask<void> {
    sum += job.cnt;
    latch.countDown();
    co_return;
  });

  for (auto i = 0; i < n; ++i) {
    pool.enqueueSync(Job{i + 1});
    folly::coro::blockingWait(pool.enqueue(Job{i}));
  }

  folly::coro::blockingWait(latch.wait());

  ASSERT_EQ(sum, 10000);
}

TEST(TestCoroutinesPool, Normal) { testCoroutinesPool(false); }

TEST(TestCoroutinesPool, WorkStealing) { testCoroutinesPool(true); }

}  // namespace
}  // namespace hf3fs::test
