#include <chrono>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

#include "common/utils/ReentrantLockManager.h"

namespace hf3fs::test {
namespace {

using namespace std::chrono_literals;

TEST(TestReentrantLockManager, Normal) {
  ReentrantLockManager lockManager;
  lockManager.init();

  auto lock1 = lockManager.writeLock(123);
  auto lock2 = lockManager.writeLock(456);

  auto lock3 = lockManager.tryWriteLock(124);
  ASSERT_TRUE(lock3.writeLocked());

  auto lock4 = lockManager.tryWriteLock(123);
  ASSERT_FALSE(lock4.writeLocked());

  lock1.unlock();

  auto lock5 = lockManager.tryWriteLock(123);
  ASSERT_TRUE(lock5.writeLocked());

  auto lock6 = lockManager.tryWriteLock(456);
  ASSERT_FALSE(lock6.writeLocked());
}

TEST(TestReentrantLockManager, SharedLock) {
  ReentrantLockManager lockManager;
  lockManager.init();

  auto lock = lockManager.readLock(123);
  std::jthread([&] { auto anotherLock = lockManager.readLock(123); });
}

TEST(TestReentrantLockManager, FairSharedLock) {
  ReentrantLockManager lockManager;
  lockManager.init();

  std::jthread read1([&] {
    // 0ms
    auto start = std::chrono::steady_clock::now();
    auto lock = lockManager.readLock(1);
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
    lock = lockManager.readLock(1);
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
    auto lock = lockManager.readLock(1);
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
    auto lock = lockManager.writeLock(1);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    XLOGF(INFO, "write lock wait {}ms", elapsed.count());

    ASSERT_NEAR(elapsed.count(), 200, 50);
  });
}

}  // namespace
}  // namespace hf3fs::test
