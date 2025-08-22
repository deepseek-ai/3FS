#include <folly/executors/CPUThreadPoolExecutor.h>

#include "common/utils/CoroSynchronized.h"
#include "common/utils/CountDownLatch.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {
folly::CPUThreadPoolExecutor &getExecutor() {
  static folly::CPUThreadPoolExecutor executor(4, std::make_shared<folly::NamedThreadFactory>("Test"));
  return executor;
}

TEST(CoroSynchronizedTest, testInc) {
  CoroSynchronized<int64_t> count(0);
  constexpr int64_t countPerAdder = 10000;
  constexpr int64_t adderCount = 10;

  CountDownLatch latch(adderCount);

  auto adder = [&]() -> CoTask<void> {
    for (int64_t i = 0; i < countPerAdder; ++i) {
      auto ptr = co_await count.coLock();
      ++*ptr;
    }
    latch.countDown();
  };

  for (int64_t i = 0; i < adderCount; ++i) {
    adder().scheduleOn(&getExecutor()).start();
  }

  auto result = folly::coro::blockingWait([&]() -> CoTask<int64_t> {
    co_await latch.wait();
    auto ptr = co_await count.coSharedLock();
    co_return *ptr;
  }());

  ASSERT_EQ(result, adderCount * countPerAdder);
}
}  // namespace
}  // namespace hf3fs::test
