#include <chrono>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <gtest/gtest.h>
#include <thread>

#include "common/net/Waiter.h"
#include "common/utils/Coroutine.h"

namespace hf3fs::net::test {
namespace {

using namespace std::chrono_literals;

TEST(TestTimer, Normal) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    Waiter::Item item;
    auto uuid = Waiter::instance().bind(item);
    auto start = std::chrono::steady_clock::now();

    Waiter::instance().schedule(uuid, std::chrono::milliseconds(50));
    co_await item.baton;

    auto elapsed = std::chrono::steady_clock::now() - start;
    CO_ASSERT_TRUE(30ms <= elapsed && elapsed <= 70ms);
    CO_ASSERT_EQ(item.status.code(), RPCCode::kTimeout);
  }());
}

TEST(TestTimer, TwoUUIDs) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    Waiter::Item item1;
    auto uuid1 = Waiter::instance().bind(item1);
    Waiter::Item item2;
    auto uuid2 = Waiter::instance().bind(item2);

    auto start = std::chrono::steady_clock::now();
    Waiter::instance().schedule(uuid1, std::chrono::milliseconds(50));
    Waiter::instance().schedule(uuid2, std::chrono::milliseconds(80));

    co_await item1.baton;
    co_await item2.baton;

    auto elapsed = std::chrono::steady_clock::now() - start;
    CO_ASSERT_TRUE(60ms <= elapsed);
    CO_ASSERT_TRUE(elapsed <= 100ms);
  }());
}

TEST(TestTimer, TimeoutBeforeWait) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    Waiter::Item item;
    auto uuid = Waiter::instance().bind(item);
    Waiter::instance().schedule(uuid, std::chrono::milliseconds(0));

    std::this_thread::sleep_for(10ms);
    co_await item.baton;
  }());
}

}  // namespace
}  // namespace hf3fs::net::test
