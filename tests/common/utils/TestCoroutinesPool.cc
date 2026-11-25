#include <atomic>
#include <chrono>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <stdexcept>
#include <thread>

#include "common/utils/CoroutinesPool.h"
#include "common/utils/CountDownLatch.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

// Polls with a deadline: without the fix the jobs never run, and waiting on a latch would block forever.
template <class Pred>
void waitFor(Pred pred) {
  for (auto i = 0; i < 100 && !pred(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

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

TEST(TestCoroutinesPool, JobThrows) {
  folly::CPUThreadPoolExecutor executor(4);

  // Declared before the pool so that they outlive its coroutines if an assertion fails early.
  std::atomic<int> thrown = 0;
  std::atomic<int> done = 0;

  int coroutines = 2;
  CoroutinesPool<int>::Config config;
  config.set_coroutines_num(coroutines);
  CoroutinesPool<int> pool(config, &executor);

  SCOPE_EXIT { pool.stopAndJoin(); };

  pool.start([&thrown, &done](int job) -> CoTask<void> {
    if (job < 0) {
      ++thrown;
      throw std::runtime_error("job failed");
    }
    ++done;
    co_return;
  });

  // One throwing job per coroutine, then normal jobs: the pool must keep running them.
  for (auto i = 0; i < coroutines; ++i) {
    pool.enqueueSync(-1);
  }
  waitFor([&thrown, coroutines] { return thrown == coroutines; });
  ASSERT_EQ(thrown, coroutines);

  int n = 100;
  for (auto i = 0; i < n; ++i) {
    pool.enqueueSync(i);
  }
  waitFor([&done, n] { return done == n; });
  ASSERT_EQ(done, n);
}

}  // namespace
}  // namespace hf3fs::test
