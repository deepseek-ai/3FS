#include <chrono>
#include <folly/Executor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <gtest/gtest.h>
#include <thread>

#include "common/utils/Coroutine.h"
#include "common/utils/PriorityUnboundedQueue.h"

namespace hf3fs::test {
namespace {

TEST(TestPriorityBoundedQueue, Normal) {
  PriorityUnboundedQueue<int> queue(3);
  ASSERT_TRUE(queue.size() == 0);

  queue.addWithPriority(1, folly::Executor::LO_PRI);
  queue.addWithPriority(2, folly::Executor::HI_PRI);
  queue.addWithPriority(3, folly::Executor::MID_PRI);
  queue.addWithPriority(4, folly::Executor::HI_PRI);

  ASSERT_EQ(queue.dequeue(), 2);
  ASSERT_EQ(queue.dequeue(), 4);
  ASSERT_EQ(queue.dequeue(), 3);
  ASSERT_EQ(queue.dequeue(), 1);
}

TEST(TestPriorityBoundedQueue, Many) {
  PriorityUnboundedQueue<int> queue(3);
  ASSERT_TRUE(queue.size() == 0);

  std::jthread a([&]() {
    for (size_t i = 0; i < 2000000; i++) {
      ASSERT_EQ(queue.dequeue(), 1);
    }
  });
  std::jthread b([&]() {
    for (size_t i = 0; i < 1000000; i++) {
      queue.addWithPriority(1, folly::Executor::HI_PRI);
      queue.addWithPriority(1, folly::Executor::LO_PRI);
    }
  });
}

}  // namespace
}  // namespace hf3fs::test