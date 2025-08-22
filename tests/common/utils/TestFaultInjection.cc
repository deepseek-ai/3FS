#include <atomic>
#include <chrono>
#include <fmt/core.h>
#include <folly/Random.h>
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
#include <folly/futures/Future.h>
#include <folly/io/async/Request.h>
#include <gtest/gtest.h>
#include <optional>
#include <type_traits>
#include <vector>

#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"

namespace hf3fs::test {
namespace {

TEST(TestFaultInjection, Basic) {
  static constexpr size_t kLoops = 100000;

  auto fi = FaultInjection::get();
  ASSERT_EQ(fi, nullptr);
  for (size_t i = 0; i < kLoops; i++) {
    auto inject = FAULT_INJECTION();
    ASSERT_FALSE(inject);
  }

  FAULT_INJECTION_SET(1, -1);  // 1% probility and unlimited times
  ASSERT_EQ(FaultInjection::get()->getProbability(), 1);
  ASSERT_EQ(FaultInjectionFactor::get(), 1);
  size_t cnt = 0;
  for (size_t i = 0; i < kLoops; i++) {
    if (FAULT_INJECTION()) {
      cnt++;
    }
  }
  ASSERT_NE(cnt, 0);
  std::cout << "Inject " << cnt << " faults during " << kLoops << " loops, " << (double)cnt / kLoops * 100
            << "%, expect 1%" << std::endl;

  {
    // disable fault injection in current scope
    FAULT_INJECTION_SET(0, -1);
    size_t cnt = 0;
    for (size_t i = 0; i < kLoops; i++) {
      if (FAULT_INJECTION()) {
        cnt++;
      }
    }
    ASSERT_EQ(cnt, 0);
  }
  ASSERT_EQ(FaultInjection::get()->getProbability(), 1);
  ASSERT_EQ(FaultInjectionFactor::get(), 1);

  // test set factor scope.
  {
    FAULT_INJECTION_SET_FACTOR(2);
    ASSERT_EQ(FaultInjectionFactor::get(), 2);
    {
      FAULT_INJECTION_SET_FACTOR(3);
      ASSERT_EQ(FaultInjectionFactor::get(), 3);
    }
    ASSERT_EQ(FaultInjectionFactor::get(), 2);

    FAULT_INJECTION_SET_FACTOR(4);
    ASSERT_EQ(FaultInjectionFactor::get(), 4);
  }
  ASSERT_EQ(FaultInjectionFactor::get(), 1);

  {
    FAULT_INJECTION_SET_FACTOR(10);
    ASSERT_EQ(FaultInjectionFactor::get(), 10);
    size_t cnt = 0;
    for (size_t i = 0; i < kLoops; i++) {
      if (FAULT_INJECTION()) {
        cnt++;
      }
    }
    ASSERT_NE(cnt, 0);
    std::cout << "Set factor of current scope to 10, inject " << cnt << " faults during " << kLoops << " loops, "
              << (double)cnt / kLoops * 100 << "%, expect 0.1%" << std::endl;
  }
  ASSERT_EQ(FaultInjectionFactor::get(), 1);

  {
    size_t cnt = 0;
    FAULT_INJECTION_SET_FACTOR(100);  // should be ignored
    for (size_t i = 0; i < kLoops; i++) {
      if (FAULT_INJECTION_WITH_FACTOR(5)) {
        cnt++;
      }
    }
    ASSERT_NE(cnt, 0);
    std::cout << "Inject with factor 5, inject " << cnt << " faults during " << kLoops << " loops, "
              << (double)cnt / kLoops * 100 << "%, expect 0.2%" << std::endl;
  }

  {
    FAULT_INJECTION_SET(10, 10);
    size_t cnt = 0;
    for (size_t i = 0; i < kLoops; i++) {
      if (FAULT_INJECTION()) {
        cnt++;
      }
    }
    ASSERT_EQ(cnt, 10);
  }
}

TEST(TestFaultInjection, Coroutine) {
  ASSERT_EQ(FaultInjection::get(), nullptr);

  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    folly::RequestContext::create();
    FAULT_INJECTION_SET(10, -1);
    co_return;
  }());
  ASSERT_EQ(FaultInjection::get(), nullptr);

  FAULT_INJECTION_SET(10, -1);
  auto fi = FaultInjection::get();
  ASSERT_NE(fi, nullptr);

  folly::coro::blockingWait([]() -> folly::coro::Task<void> {
    folly::RequestContext::create();
    FAULT_INJECTION_SET(10, -1);
    co_return;
  }());
  ASSERT_EQ(FaultInjection::get(), fi);

  {
    folly::CPUThreadPoolExecutor executor(4);
    folly::coro::co_invoke([fi]() -> folly::coro::Task<void> {
      CO_ASSERT_EQ(FaultInjection::get(), fi);
      co_return;
    })
        .scheduleOn(&executor)
        .start()
        .wait();
    executor.join();
  }

  {
    FAULT_INJECTION_SET(10, 1000);
    folly::CPUThreadPoolExecutor executor(4);
    std::vector<folly::SemiFuture<size_t>> futures;
    for (int i = 0; i < 10; i++) {
      auto future = folly::coro::co_invoke([&]() -> folly::coro::Task<size_t> {
                      size_t cnts = 0;
                      for (int i = 0; i < 100000; i++) {
                        if (FAULT_INJECTION()) {
                          cnts++;
                        }
                        if (folly::Random::oneIn(100)) co_await folly::coro::co_reschedule_on_current_executor_t();
                      }
                      co_return cnts;
                    })
                        .scheduleOn(&executor)
                        .start();
      futures.push_back(std::move(future));
    }
    auto results = folly::collectAll(futures.begin(), futures.end()).wait().result().value();
    size_t cnts = 0;
    for (auto &result : results) {
      cnts += result.value();
    }
    executor.join();
    ASSERT_EQ(cnts, 1000);
  }
}

TEST(TestFaultInjection, Coroutine1) {
  folly::CPUThreadPoolExecutor exec(1);
  auto coro1 = [](FaultInjection *ptr) -> CoTask<void> {
    CO_ASSERT_EQ(FaultInjection::get(), ptr);
    co_return;
  };
  auto coro2 = [&]() -> CoTask<void> {
    CO_ASSERT_EQ(FaultInjection::get(), nullptr);
    FAULT_INJECTION_SET(10, 1);
    co_await coro1(FaultInjection::get()).scheduleOn(&exec).start();
    co_return;
  };
  coro2().scheduleOn(&exec).start().wait();
  sleep(1);
}

TEST(TestFaultInjection, Coroutine2) {
  folly::CPUThreadPoolExecutor exec(1);
  auto coro1 = []() -> CoTask<void> {
    CO_ASSERT_EQ(folly::RequestContext::try_get(), nullptr);
    CO_ASSERT_EQ(FaultInjection::get(), nullptr);
    folly::RequestContext::create();
    FAULT_INJECTION_SET(10, 1);
    CO_ASSERT_NE(folly::RequestContext::try_get(), nullptr);
    CO_ASSERT_NE(FaultInjection::get(), nullptr);
    co_return;
  };
  auto coro2 = [&]() -> CoTask<void> {
    CO_ASSERT_EQ(folly::RequestContext::try_get(), nullptr);
    CO_ASSERT_EQ(FaultInjection::get(), nullptr);
    co_await coro1().scheduleOn(&exec).start();
    CO_ASSERT_EQ(folly::RequestContext::try_get(), nullptr);
    CO_ASSERT_EQ(FaultInjection::get(), nullptr);
    co_return;
  };
  coro2().scheduleOn(&exec).start().wait();
}

}  // namespace
}  // namespace hf3fs::test