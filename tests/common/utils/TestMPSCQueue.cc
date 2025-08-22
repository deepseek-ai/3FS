#include <gtest/gtest.h>
#include <memory>

#include "common/utils/MPSCQueue.h"

namespace hf3fs::test {
namespace {

TEST(TestMPSCQueue, Normal) {
  constexpr auto N = 4096u;

  MPSCQueue<size_t> que(N);
  for (auto i = 0u; i < N; ++i) {
    auto item = std::make_unique<size_t>(i);
    ASSERT_TRUE(que.push(&item));
    ASSERT_TRUE(item == nullptr);
  }
  auto more = std::make_unique<size_t>(0);
  ASSERT_EQ(que.push(&more).code(), StatusCode::kQueueFull);
  ASSERT_TRUE(more != nullptr);

  for (auto i = 0u; i < N; ++i) {
    std::unique_ptr<size_t> item;
    ASSERT_TRUE(que.pop(&item));
    ASSERT_TRUE(item != nullptr);
  }
  ASSERT_EQ(que.pop(&more).code(), StatusCode::kQueueEmpty);
}

}  // namespace
}  // namespace hf3fs::test
