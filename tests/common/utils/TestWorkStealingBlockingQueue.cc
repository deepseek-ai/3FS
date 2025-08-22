#include <folly/executors/task_queue/UnboundedBlockingQueue.h>

#include "common/utils/WorkStealingBlockingQueue.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {
template <template <typename, typename> typename QueueTemplate>
auto createQueues(size_t count = 10) {
  auto isEnd = [](const int &x) { return x == 0; };
  using WSQueue = QueueTemplate<int, decltype(isEnd)>;

  auto sharedState = std::make_shared<typename WSQueue::SharedState>(count);
  std::vector<std::unique_ptr<WSQueue>> wsQueues;
  for (size_t i = 0; i < count; ++i) {
    wsQueues.push_back(std::make_unique<WSQueue>(sharedState, i, isEnd));
  }
  return wsQueues;
}

TEST(WorkStealingBlockingQueue, testWorkStealing) {
  auto wsQueues = createQueues<WorkStealingBlockingQueue>();

  wsQueues[0]->add(5);
  ASSERT_EQ(wsQueues[1]->take(), 5);
  ASSERT_TRUE(!wsQueues[0]->try_take_for(std::chrono::milliseconds(100)).hasValue());

  wsQueues[2]->add(10);
  for (;;) {
    auto res = wsQueues[3]->try_take_for(std::chrono::milliseconds(100));
    if (res) {
      ASSERT_EQ(*res, 10);
      break;
    }
  }

  wsQueues[2]->add(0);
  auto res = wsQueues[3]->try_take_for(std::chrono::milliseconds(100));
  ASSERT_TRUE(!res);

  for (;;) {
    res = wsQueues[2]->try_take_for(std::chrono::milliseconds(0));
    if (res) break;
  }
  ASSERT_EQ(*res, 0);
}

TEST(WorkStealingBlockingQueue, testRoundRobin) {
  auto wsQueues = createQueues<RoundRobinBlockingQueue>(4);

  wsQueues[0]->add(5);
  ASSERT_EQ(wsQueues[1]->take(), 5);
  ASSERT_TRUE(!wsQueues[0]->try_take_for(std::chrono::milliseconds(100)).hasValue());

  wsQueues[2]->add(10);
  for (;;) {
    auto res = wsQueues[3]->try_take_for(std::chrono::milliseconds(100));
    if (res) {
      ASSERT_EQ(*res, 10);
      break;
    }
  }

  wsQueues[2]->add(0);
  auto res = wsQueues[3]->try_take_for(std::chrono::milliseconds(100));
  ASSERT_TRUE(!res);

  for (;;) {
    res = wsQueues[2]->try_take_for(std::chrono::milliseconds(0));
    if (res) break;
  }
  ASSERT_EQ(*res, 0);
}

TEST(WorkStealingBlockingQueue, testSharedNothing) {
  auto wsQueues = createQueues<SharedNothingBlockingQueue>();

  wsQueues[0]->add(5);
  ASSERT_TRUE(!wsQueues[1]->try_take_for(std::chrono::milliseconds(100)));
}
}  // namespace
}  // namespace hf3fs::test
