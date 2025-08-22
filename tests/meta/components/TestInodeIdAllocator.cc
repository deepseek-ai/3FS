#include <chrono>
#include <folly/Synchronized.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <gtest/gtest-typed-test.h>
#include <memory>
#include <vector>

#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "meta/components/InodeIdAllocator.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server::test {

template <typename KV>
class TestInodeIdAllocator : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestInodeIdAllocator, KVTypes);

CoTask<void> doAllocate(std::shared_ptr<InodeIdAllocator> allocator,
                        size_t times,
                        folly::Synchronized<std::vector<InodeId>, std::mutex> &allocated,
                        bool allowFailure = false) {
  for (size_t i = 1; i <= times; i++) {
    // since there is only one process, allocate shouldn't fail.
    auto result = co_await allocator->allocate(std::chrono::seconds(10));
    if (!allowFailure) {
      CO_ASSERT_OK(result);
    }
    if (result.hasError()) {
      CO_ASSERT_ERROR(result, MetaCode::kInodeIdAllocFailed);
    } else {
      allocated.lock()->push_back(result.value());
      co_await folly::coro::co_reschedule_on_current_executor;
    }
  }
  co_return;
}

TYPED_TEST(TestInodeIdAllocator, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto kv = this->kvEngine();
    auto allocator = InodeIdAllocator::create(kv);

    std::vector<InodeId> allocated;
    for (size_t i = 0; i < 10000; i++) {
      auto result = co_await allocator->allocate();
      CO_ASSERT_OK(result);
      allocated.push_back(result.value());
    }

    std::sort(allocated.begin(), allocated.end());
    for (size_t i = 1; i < allocated.size(); i++) {
      CO_ASSERT_GT(allocated.at(i), allocated.at(i - 1));
    }
    fmt::print("first {}, last {}\n", allocated.begin()->toHexString(), allocated.rbegin()->toHexString());
  }());
}

TYPED_TEST(TestInodeIdAllocator, MultiThreads) {
  folly::Synchronized<std::vector<InodeId>, std::mutex> allocated;
  std::vector<folly::SemiFuture<folly::Unit>> futures;
  folly::CPUThreadPoolExecutor exec(8);

  auto kv = this->kvEngine();
  auto allocator = InodeIdAllocator::create(kv);

  for (size_t i = 0; i < 32; i++) {
    // since there is only one process, allocate shouldn't fail.
    futures.push_back(doAllocate(allocator, 10000, allocated, false).scheduleOn(&exec).start());
  }
  for (auto &f : futures) {
    f.wait();
  }

  auto lock = allocated.lock();
  std::sort(lock->begin(), lock->end());
  fmt::print("succ {}, min {}, max {}\n", lock->size(), lock->begin()->toHexString(), lock->rbegin()->toHexString());
  for (size_t i = 1; i < lock->size(); i++) {
    ASSERT_GT(lock->at(i), lock->at(i - 1));
  }
}

TYPED_TEST(TestInodeIdAllocator, MultiAllocator) {
  folly::Synchronized<std::vector<InodeId>, std::mutex> allocated;
  std::vector<folly::SemiFuture<folly::Unit>> futures;
  folly::CPUThreadPoolExecutor exec(8);

  auto kv = this->kvEngine();
  std::vector<std::shared_ptr<InodeIdAllocator>> vec;
  vec.reserve(32);
  for (size_t i = 0; i < 32; i++) {
    vec.push_back(InodeIdAllocator::create(kv));
  }
  for (size_t i = 0; i < 128; i++) {
    auto &allocator = vec[i % vec.size()];
    futures.push_back(doAllocate(allocator, 10000, allocated, true).scheduleOn(&exec).start());
  }
  for (auto &f : futures) {
    f.wait();
  }

  auto lock = allocated.lock();
  std::sort(lock->begin(), lock->end());
  fmt::print("succ {}, min {}, max {}\n", lock->size(), lock->begin()->toHexString(), lock->rbegin()->toHexString());
  for (size_t i = 1; i < lock->size(); i++) {
    ASSERT_GT(lock->at(i), lock->at(i - 1));
  }
}

TYPED_TEST(TestInodeIdAllocator, FaultInjection) {
  folly::Synchronized<std::vector<InodeId>, std::mutex> allocated;
  std::vector<folly::SemiFuture<folly::Unit>> futures;
  folly::CPUThreadPoolExecutor exec(8);

  auto kv = this->kvEngine();
  auto allocator = InodeIdAllocator::create(kv);

  for (size_t i = 0; i < 8; i++) {
    FAULT_INJECTION_SET(10, -1);
    futures.push_back(doAllocate(allocator, 10000, allocated, true).scheduleOn(&exec).start());
  }
  for (size_t i = 0; i < 8; i++) {
    futures.push_back(doAllocate(allocator, 10000, allocated, false).scheduleOn(&exec).start());
  }
  for (auto &f : futures) {
    f.wait();
  }

  auto lock = allocated.lock();
  std::sort(lock->begin(), lock->end());
  fmt::print("succ {}, min {}, max {}\n", lock->size(), lock->begin()->toHexString(), lock->rbegin()->toHexString());
  for (size_t i = 1; i < lock->size(); i++) {
    ASSERT_GT(lock->at(i), lock->at(i - 1));
  }
}

}  // namespace hf3fs::meta::server::test
