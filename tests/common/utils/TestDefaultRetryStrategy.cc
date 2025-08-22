#include <folly/experimental/coro/BlockingWait.h>
#include <gtest/gtest.h>

#include "common/utils/DefaultRetryStrategy.h"

namespace hf3fs::tests {
namespace {
using namespace std::chrono_literals;

struct MockSleeper {
  CoTask<void> operator()(std::chrono::milliseconds d) {
    records.push_back(d);
    co_return;
  }

  std::vector<std::chrono::milliseconds> records;
};

DefaultRetryStrategy<MockSleeper> getDefaultRetryStrategy() {
  return DefaultRetryStrategy<MockSleeper>(RetryConfig{10ms, 100ms, 5});
}

TEST(DefaultRetryStrategy, basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    {
      auto retryStrategy = getDefaultRetryStrategy();
      // kDataCorruption is unretryable
      auto r = co_await retryStrategy.onError(Status(StatusCode::kDataCorruption));
      EXPECT_TRUE(r.hasError());
      EXPECT_EQ(r.error().code(), StatusCode::kDataCorruption);
      EXPECT_EQ(retryStrategy.getSleeper().records.size(), 0);
    }
    {
      auto retryStrategy = getDefaultRetryStrategy();
      // kThrottled is retryable
      for (int i = 0; i < 5; ++i) {
        auto r = co_await retryStrategy.onError(Status(TransactionCode::kThrottled));
        EXPECT_TRUE(!r.hasError());
      }
      {
        auto r = co_await retryStrategy.onError(Status(TransactionCode::kThrottled));
        EXPECT_TRUE(r.hasError());
        EXPECT_EQ(r.error().code(), TransactionCode::kThrottled);
      }
      std::vector<std::chrono::milliseconds> expected = {10ms, 20ms, 40ms, 80ms, 100ms};
      EXPECT_EQ(retryStrategy.getSleeper().records, expected);
    }
  }());
}

}  // namespace
}  // namespace hf3fs::tests
