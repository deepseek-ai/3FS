#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/Baton.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/Timeout.h>
#include <gtest/gtest.h>

#include "common/utils/CoLockManager.h"
#include "common/utils/Duration.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

using namespace std::chrono_literals;

TEST(TestCoLockManager, Normal) {
  CoLockManager lockManager;
  {
    folly::coro::Baton baton;
    auto guard = lockManager.lock(baton, "A", "no wait");
    folly::coro::blockingWait(guard.lock());
  }
  {
    folly::coro::Baton baton;
    auto guard = lockManager.lock(baton, "A", "no wait");
    folly::coro::blockingWait(guard.lock());
  }

  folly::CPUThreadPoolExecutor executor(1);
  folly::coro::co_invoke([&]() -> CoTask<void> {
    folly::coro::Baton baton;
    auto guard = lockManager.lock(baton, "B", "first");
    CO_ASSERT_TRUE(guard.locked());
    co_await guard.lock();
    co_await folly::coro::sleep(200ms);
    co_return;
  })
      .scheduleOn(&executor)
      .start();

  folly::coro::co_invoke([&]() -> CoTask<void> {
    co_await folly::coro::sleep(100ms);
    folly::coro::Baton baton;
    auto start = RelativeTime::now();
    auto guard = lockManager.lock(baton, "B", "second");
    CO_ASSERT_FALSE(guard.locked());
    co_await guard.lock();
    auto elapsed = RelativeTime::now() - start;
    CO_ASSERT_TRUE(70_ms <= elapsed);
    CO_ASSERT_TRUE(elapsed <= 130_ms);
    co_await folly::coro::sleep(200ms);
    co_return;
  })
      .scheduleOn(&executor)
      .start();

  {
    std::this_thread::sleep_for(300ms);
    folly::coro::Baton baton;
    auto start = RelativeTime::now();
    auto guard = lockManager.lock(baton, "B", "third");
    ASSERT_FALSE(guard.locked());
    folly::coro::blockingWait(guard.lock());
    auto elapsed = RelativeTime::now() - start;
    ASSERT_NEAR(elapsed.asMs().count(), 100, 30);
  }
}

TEST(TestCoLockManager, Concurrent) {
  CoLockManager lockManager;
  folly::CPUThreadPoolExecutor executor(16);
  constexpr auto N = 20000;
  auto cnt = 0;
  auto current = 0;
  folly::coro::Baton finish;
  for (auto i = 0; i < N; ++i) {
    folly::coro::co_invoke([&]() -> CoTask<void> {
      folly::coro::Baton baton;
      auto guard = lockManager.lock(baton, "A");
      co_await guard.lock();
      ++current;
      CO_ASSERT_EQ(current, 1);
      if (++cnt == N) {
        finish.post();
      }
      --current;
    })
        .scheduleOn(&executor)
        .start();
  }
  auto result = folly::coro::blockingWait(
      folly::coro::co_awaitTry(folly::coro::timeout([&]() -> CoTask<void> { co_await finish; }(), 5s)));
  ASSERT_FALSE(result.hasException());
}

TEST(TestCoLockManager, Concurrent2) {
  CoLockManager lockManager;
  constexpr auto N = 20000;
  auto cnt = 0;
  auto current = 0;
  folly::coro::Baton finish;
  std::vector<std::thread> threads(16);
  for (auto &thread : threads) {
    thread = std::thread([&] {
      for (auto i = 0; i < N / 16; ++i) {
        folly::coro::Baton baton;
        auto guard = lockManager.lock(baton, "A");
        if (!guard.locked()) {
          folly::coro::blockingWait(guard.lock());
        }
        ++current;
        ASSERT_EQ(current, 1);
        if (++cnt == N) {
          finish.post();
        }
        --current;
      }
    });
  }
  auto result = folly::coro::blockingWait(
      folly::coro::co_awaitTry(folly::coro::timeout([&]() -> CoTask<void> { co_await finish; }(), 5s)));
  ASSERT_FALSE(result.hasException());
  for (auto &thread : threads) {
    thread.join();
  }
}

TEST(TestCoLockManager, Concurrent3) {
  CoLockManager lockManager;
  constexpr auto N = 20000;
  auto cnt = 0;
  auto current = 0;
  folly::coro::Baton finish;
  std::vector<std::thread> threads(16);
  for (auto &thread : threads) {
    thread = std::thread([&] {
      for (auto i = 0; i < N / 16;) {
        folly::coro::Baton baton;
        auto guard = lockManager.tryLock(baton, "A");
        if (!guard.locked()) {
          continue;
        }

        ++current;
        ASSERT_EQ(current, 1);
        if (++cnt == N) {
          finish.post();
        }
        --current;
        ++i;
      }
    });
  }
  auto result = folly::coro::blockingWait(
      folly::coro::co_awaitTry(folly::coro::timeout([&]() -> CoTask<void> { co_await finish; }(), 5s)));
  ASSERT_FALSE(result.hasException());
  for (auto &thread : threads) {
    thread.join();
  }
}

}  // namespace
}  // namespace hf3fs::test
