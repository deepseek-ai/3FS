#include <chrono>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/FutureUtil.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/Promise.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GTest.h>
#include <thread>
#include <vector>

TEST(TestEventBase, Normal) {
  folly::EventBase evb;
  bool finished = false;

  folly::coro::co_invoke([&]() -> folly::coro::Task<void> {
    auto executor = co_await folly::coro::co_current_executor;
    EXPECT_EQ(executor, &evb);
    finished = true;
    evb.terminateLoopSoon();
    co_return;
  })
      .scheduleOn(&evb)
      .start();

  evb.loopForever();
  ASSERT_TRUE(finished);
}
