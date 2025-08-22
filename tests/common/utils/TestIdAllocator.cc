#include <algorithm>
#include <atomic>
#include <chrono>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/Synchronized.h>
#include <folly/Unit.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/futures/Future.h>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <vector>

#include "common/kv/mem/MemKVEngine.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/IdAllocator.h"
#include "fdb/FDBRetryStrategy.h"
#include "fdb/FDBTransaction.h"
#include "tests/GtestHelpers.h"
namespace hf3fs::test {

kv::FDBRetryStrategy retryStrategy() { return kv::FDBRetryStrategy({.retryMaybeCommitted = true}); }

TEST(TestIdAllocator, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    kv::MemKVEngine engine;
    IdAllocator<kv::FDBRetryStrategy> allocator(engine, retryStrategy(), "test", 1);

    for (size_t i = 1; i <= 1000; i++) {
      auto result = co_await allocator.allocate();
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result.value(), i);
    }
  }());
}

TEST(TestIdAllocator, MultiThreads) {
  kv::MemKVEngine engine;
  IdAllocator<kv::FDBRetryStrategy> allocator(engine, retryStrategy(), "test", 1);

  std::atomic<size_t> failed = 0;
  folly::Synchronized<std::vector<uint64_t>, std::mutex> allocated;
  std::vector<folly::SemiFuture<folly::Unit>> futures;
  folly::CPUThreadPoolExecutor exec(8);
  for (size_t i = 0; i < 128; i++) {
    auto task = [&]() -> CoTask<void> {
      for (size_t i = 1; i <= 1000; i++) {
        auto result = co_await allocator.allocate();
        if (result.hasError()) {
          CO_ASSERT_ERROR(result, TransactionCode::kConflict);
          failed++;
        } else {
          allocated.lock()->push_back(result.value());
        }
        co_await folly::coro::co_reschedule_on_current_executor;
      }
      co_return;
    };
    futures.push_back(folly::coro::co_invoke(task).scheduleOn(&exec).start());
  }

  for (auto &f : futures) {
    f.wait();
  }

  auto lock = allocated.lock();
  std::sort(lock->begin(), lock->end());
  fmt::print("success {}, failed {}, first {}, last {}\n",
             lock->size(),
             failed.load(),
             *lock->begin(),
             *lock->rbegin());
  for (size_t i = 1; i < lock->size(); i++) {
    ASSERT_GT(lock->at(i), lock->at(i - 1));
  }
}

TEST(TestIdAllocator, Sharded) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    kv::MemKVEngine engine;
    IdAllocator<kv::FDBRetryStrategy> allocator(engine, retryStrategy(), "test", 8);

    std::vector<uint64_t> allocated;
    for (size_t i = 1; i <= 1000; i++) {
      auto result = co_await allocator.allocate();
      CO_ASSERT_OK(result);
      allocated.push_back(result.value());
    }

    std::sort(allocated.begin(), allocated.end());
    for (size_t i = 1; i < allocated.size(); i++) {
      CO_ASSERT_GT(allocated.at(i), allocated.at(i - 1));
    }

    auto status = co_await allocator.status();
    CO_ASSERT_OK(status);
    std::sort(status->begin(), status->end());
    fmt::print("shard usage: min {}, max {}\n", *status->begin(), *status->rbegin());
  }());
}

TEST(TestIdAllocator, ShardedMultiThreads) {
  kv::MemKVEngine engine;
  std::vector<std::unique_ptr<IdAllocator<kv::FDBRetryStrategy>>> vec;
  for (int i = 0; i < 8; i++) {
    vec.emplace_back(std::make_unique<IdAllocator<kv::FDBRetryStrategy>>(engine, retryStrategy(), "test", 32));
  }

  std::atomic<size_t> failed = 0;
  folly::Synchronized<std::vector<uint64_t>, std::mutex> allocated;
  std::vector<folly::SemiFuture<folly::Unit>> futures;
  folly::CPUThreadPoolExecutor exec(8);
  for (size_t i = 0; i < 128; i++) {
    auto task = [&, &allocator = vec[i % vec.size()]]() -> CoTask<void> {
      for (size_t i = 1; i <= 1000; i++) {
        auto result = co_await allocator->allocate();
        if (result.hasError()) {
          CO_ASSERT_ERROR(result, TransactionCode::kConflict);
          failed++;
        } else {
          allocated.lock()->push_back(result.value());
        }
        co_await folly::coro::co_reschedule_on_current_executor;
      }
      co_return;
    };
    futures.push_back(folly::coro::co_invoke(task).scheduleOn(&exec).start());
  }

  for (auto &f : futures) {
    f.wait();
  }

  auto lock = allocated.lock();
  std::sort(lock->begin(), lock->end());
  fmt::print("success {}, failed {}, min {}, max {}\n", lock->size(), failed.load(), *lock->begin(), *lock->rbegin());
  for (size_t i = 1; i < lock->size(); i++) {
    ASSERT_GT(lock->at(i), lock->at(i - 1));
  }

  auto status = vec[0]->status().scheduleOn(&exec).start().wait().value();
  ASSERT_OK(status);
  std::sort(status->begin(), status->end());
  fmt::print("shard usage: min {}, max {}\n", *status->begin(), *status->rbegin());
}

TEST(TestIdAllocator, FaultInjection) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    FAULT_INJECTION_SET(2, -1);
    kv::MemKVEngine engine;
    IdAllocator<kv::FDBRetryStrategy> allocator(engine, retryStrategy(), "test", 8);
    std::set<uint64_t> set;
    size_t fail = 0;
    for (size_t i = 1; i <= 1000; i++) {
      auto result = co_await allocator.allocate();
      if (result.hasValue()) {
        CO_ASSERT_FALSE(set.contains(result.value()));
        set.insert(result.value());
      } else {
        fail++;
      }
    }

    std::cout << "success " << set.size() << ", fail " << fail << std::endl;
  }());
}

}  // namespace hf3fs::test
