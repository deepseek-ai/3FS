#include <atomic>
#include <chrono>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/concurrency/UnboundedQueue.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/experimental/coro/AsyncGenerator.h>
#include <folly/experimental/coro/Baton.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/BoundedQueue.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/ScopeExit.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/Task.h>
#include <folly/experimental/coro/UnboundedQueue.h>
#include <folly/fibers/Semaphore.h>
#include <folly/futures/Future.h>
#include <folly/io/async/Request.h>
#include <gtest/gtest.h>
#include <optional>
#include <type_traits>
#include <vector>

#include "common/utils/Coroutine.h"
namespace hf3fs::test {

namespace {

TEST(TestRequestContext, FibersBaton) {
  folly::CPUThreadPoolExecutor exec(folly::Random::rand32(1) + 1);
  folly::fibers::Baton baton;
  auto producer = folly::coro::co_invoke([&]() -> CoTask<void> {
                    co_await folly::coro::sleep(std::chrono::seconds(1));
                    folly::RequestContext::create();
                    baton.post();
                    co_return;
                  })
                      .scheduleOn(&exec)
                      .start();
  auto consumer = folly::coro::co_invoke([&]() -> CoTask<void> {
                    EXPECT_EQ(folly::RequestContext::try_get(), nullptr);
                    co_await baton;
                    EXPECT_EQ(folly::RequestContext::try_get(), nullptr);
                  })
                      .scheduleOn(&exec)
                      .start();
  producer.wait();
  consumer.wait();
}

TEST(TestRequestContext, CoroBaton) {
  folly::CPUThreadPoolExecutor exec(folly::Random::rand32(1) + 1);
  folly::coro::Baton baton;
  auto producer = folly::coro::co_invoke([&]() -> CoTask<void> {
                    co_await folly::coro::sleep(std::chrono::seconds(1));
                    folly::RequestContext::create();
                    baton.post();
                    co_return;
                  })
                      .scheduleOn(&exec)
                      .start();
  auto consumer = folly::coro::co_invoke([&]() -> CoTask<void> {
                    EXPECT_EQ(folly::RequestContext::try_get(), nullptr);
                    co_await baton;
                    EXPECT_EQ(folly::RequestContext::try_get(), nullptr);
                  })
                      .scheduleOn(&exec)
                      .start();
  producer.wait();
  consumer.wait();
}

TEST(TestRequestContext, Semaphore) {
  folly::CPUThreadPoolExecutor exec(folly::Random::rand32(1, 8));
  folly::fibers::Semaphore semaphore(folly::Random::rand32(1, 8));
  auto worker = [&]() -> CoTask<void> {
    for (size_t i = 0; i < folly::Random::rand32(1000, 2000); i++) {
      std::optional<folly::ShallowCopyRequestContextScopeGuard> guard;
      if (folly::Random::oneIn(2)) {
        guard.emplace();
      }
      auto ctx = folly::RequestContext::try_get();
      co_await semaphore.co_wait();
      SCOPE_EXIT { semaphore.signal(); };
      CO_ASSERT_EQ(ctx, folly::RequestContext::try_get());
    }
  };

  std::vector<folly::SemiFuture<Void>> tasks;
  for (size_t i = 0; i < 64; i++) {
    tasks.push_back(folly::coro::co_invoke(worker).scheduleOn(&exec).start());
  }
  for (auto &task : tasks) {
    task.wait();
  }
}

}  // namespace
}  // namespace hf3fs::test