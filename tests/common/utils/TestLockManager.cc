#include <chrono>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/logging/xlog.h>
#include <mutex>
#include <thread>

#include "common/utils/LockManager.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

using namespace std::chrono_literals;

TEST(TestLockManager, Normal) {
  UniqueLockManager lockManager;

  auto lock1 = lockManager.lock(123);
  auto lock2 = lockManager.lock(std::string{"yes"});

  auto lock3 = lockManager.try_lock(124);
  ASSERT_TRUE(lock3.owns_lock());

  auto lock4 = lockManager.try_lock(123);
  ASSERT_FALSE(lock4.owns_lock());

  lock1.unlock();

  auto lock5 = lockManager.try_lock(123);
  ASSERT_TRUE(lock5.owns_lock());

  auto lock6 = lockManager.try_lock(std::string{"yes"});
  ASSERT_FALSE(lock6.owns_lock());
}

TEST(TestLockManager, SharedLock) {
  SharedLockManager lockManager;

  auto lock = lockManager.lock_shared(123);
  std::jthread([&] { auto anotherLock = lockManager.lock_shared(123); });
}

TEST(TestLockManager, FairSharedLock) {
  SharedLockManager lockManager;

  std::jthread read1([&] {
    // 0ms
    auto start = std::chrono::steady_clock::now();
    auto lock = lockManager.lock_shared(1);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    XLOGF(INFO, "read1 lock wait {}ms", elapsed.count());
    std::this_thread::sleep_for(100ms);

    // 100ms
    std::this_thread::sleep_for(100ms);

    // 200ms
    std::this_thread::sleep_for(100ms);

    // 300ms
    lock.unlock();
    std::this_thread::sleep_for(100ms);

    // 400ms
    lock.lock();
    std::this_thread::sleep_for(100ms);

    // 500ms
    std::this_thread::sleep_for(100ms);

    // 600ms
  });

  std::jthread read2([&] {
    // 0ms
    std::this_thread::sleep_for(100ms);

    // 100ms
    std::this_thread::sleep_for(100ms);

    // 200ms
    auto start = std::chrono::steady_clock::now();
    auto lock = lockManager.lock_shared(1);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    XLOGF(INFO, "read2 lock wait {}ms", elapsed.count());
    std::this_thread::sleep_for(100ms);

    // 300ms
    std::this_thread::sleep_for(100ms);

    // 400ms
    std::this_thread::sleep_for(100ms);

    // 500ms
    lock.unlock();
  });

  std::jthread write([&] {
    // 0ms
    std::this_thread::sleep_for(100ms);

    // 100ms
    auto start = std::chrono::steady_clock::now();
    auto lock = lockManager.lock(1);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    XLOGF(INFO, "write lock wait {}ms", elapsed.count());

    ASSERT_NEAR(elapsed.count(), 200, 50);
  });
}

TEST(TestLockManager, CoMutex) {
  CoUniqueLockManager lockManager;

  folly::coro::blockingWait([&]() -> folly::coro::Task<void> {
    auto lock1 = co_await lockManager.co_scoped_lock(123);
    auto lock2 = co_await lockManager.co_scoped_lock(std::string{"yes"});

    auto lock3 = lockManager.try_lock(124);
    CO_ASSERT_TRUE(lock3.owns_lock());

    auto lock4 = lockManager.try_lock(123);
    CO_ASSERT_FALSE(lock4.owns_lock());

    lock1.unlock();

    auto lock5 = lockManager.try_lock(123);
    CO_ASSERT_TRUE(lock5.owns_lock());

    auto lock6 = lockManager.try_lock(std::string{"yes"});
    CO_ASSERT_FALSE(lock6.owns_lock());
  }());
}

}  // namespace
}  // namespace hf3fs::test
