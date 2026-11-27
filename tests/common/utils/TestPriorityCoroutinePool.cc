#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/PriorityCoroutinePool.h"
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

TEST(TestPriorityCoroutinePool, JobThrows) {
  CPUExecutorGroup executor(8, "");

  // Declared before the pool so that they outlive its coroutines if an assertion fails early.
  std::atomic<int> thrown = 0;
  std::atomic<int> done = 0;

  int coroutines = 2;
  PriorityCoroutinePool<int>::Config config;
  config.set_coroutines_num(coroutines);
  PriorityCoroutinePool<int> pool(config);

  SCOPE_EXIT { pool.stopAndJoin(); };

  pool.start(
      [&thrown, &done](int job) -> CoTask<void> {
        if (job < 0) {
          ++thrown;
          throw std::runtime_error("job failed");
        }
        ++done;
        co_return;
      },
      executor);

  // One throwing job per coroutine, then normal jobs: the pool must keep running them.
  for (auto i = 0; i < coroutines; ++i) {
    pool.enqueue(-1, 0);
  }
  waitFor([&thrown, coroutines] { return thrown == coroutines; });
  ASSERT_EQ(thrown, coroutines);

  int n = 100;
  for (auto i = 0; i < n; ++i) {
    pool.enqueue(i, 0);
  }
  waitFor([&done, n] { return done == n; });
  ASSERT_EQ(done, n);
}

}  // namespace
}  // namespace hf3fs::test
