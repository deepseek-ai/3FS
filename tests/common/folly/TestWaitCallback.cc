#include <chrono>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/FutureUtil.h>
#include <folly/experimental/coro/Promise.h>
#include <folly/portability/GTest.h>
#include <thread>
#include <vector>

using namespace folly::coro;

TEST(CoroWaitCallbackTest, WaitCallback) {
  bool same_thread = false;

  auto task = [&same_thread]() -> folly::coro::Task<void> {
    auto tid_before = std::this_thread::get_id();
    auto [promise, future] = makePromiseContract<void>();
    std::thread notify_at_another_thread([promise = std::move(promise)]() mutable {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      promise.setValue();
    });
    co_await toTask(std::move(future));
    auto tid_after = std::this_thread::get_id();
    same_thread = tid_before == tid_after;
    notify_at_another_thread.join();
  };

  blockingWait(task());
  ASSERT_TRUE(same_thread);
}

TEST(CoroWaitCallbackTest, MultiTasks) {
  int orders = 0;
  auto create_task = [&orders]() -> folly::coro::Task<void> {
    auto [promise, future] = makePromiseContract<void>();
    std::thread notify_at_another_thread([promise = std::move(promise)]() mutable {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      promise.setValue();
    });
    orders = orders * 10 + 1;
    co_await toTask(std::move(future));
    orders = orders * 10 + 2;
    notify_at_another_thread.join();
  };

  blockingWait(collectAll(create_task(), create_task()));
  ASSERT_EQ(orders, 1122);
}
