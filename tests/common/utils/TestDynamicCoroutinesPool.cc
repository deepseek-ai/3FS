#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/Baton.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/Sleep.h>

#include "common/utils/Duration.h"
#include "common/utils/DynamicCoroutinesPool.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

TEST(TestDynamicCoroutinesPool, Normal) {
  DynamicCoroutinesPool::Config config;
  config.set_coroutines_num(8);
  DynamicCoroutinesPool pool(config);
  ASSERT_OK(pool.start());
  ASSERT_OK(pool.stopAndJoin());

  DynamicCoroutinesPool{config};
}

TEST(TestDynamicCoroutinesPool, ManyTasks) {
  constexpr auto N = 200;
  constexpr auto M = 64;
  DynamicCoroutinesPool::Config config;
  config.set_coroutines_num(M);
  DynamicCoroutinesPool pool(config);
  ASSERT_OK(pool.start());

  folly::coro::Baton baton;
  std::atomic<uint32_t> cnt{};
  std::atomic<uint32_t> current{};
  for (auto i = 0; i < N; ++i) {
    pool.enqueue(folly::coro::co_invoke([&]() -> CoTask<void> {
      auto now = ++current;
      CO_ASSERT_LE(now, M);
      co_await folly::coro::sleep(50_ms);
      if (++cnt == N) {
        baton.post();
      }
      --current;
    }));
  }

  folly::coro::blockingWait(baton);
}

TEST(TestDynamicCoroutinesPool, HotUpdated) {
  constexpr auto N = 64;
  DynamicCoroutinesPool::Config config;
  config.set_coroutines_num(1);
  DynamicCoroutinesPool pool(config);
  ASSERT_OK(pool.start());

  folly::coro::Baton baton;
  std::atomic<uint32_t> cnt{};
  std::atomic<uint32_t> current{};
  for (auto i = 0; i < N; ++i) {
    pool.enqueue(folly::coro::co_invoke([&]() -> CoTask<void> {
      auto now = ++current;
      config.set_coroutines_num(now + 1);
      config.update(toml::table{}, true);
      co_await folly::coro::sleep(50_ms);
      if (++cnt == N) {
        baton.post();
      }
      --current;
    }));
  }

  folly::coro::blockingWait(baton);
}

}  // namespace
}  // namespace hf3fs::test
