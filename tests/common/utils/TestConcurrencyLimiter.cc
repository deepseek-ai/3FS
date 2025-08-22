#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Timeout.h>

#include "common/utils/ConcurrencyLimiter.h"
#include "common/utils/Duration.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::net::test {
namespace {

TEST(TestConcurrencyLimiter, Normal) {
  ConcurrencyLimiterConfig config;
  ConcurrencyLimiter<std::string> limiter(config);
  for (auto c = 1; c <= 4; c *= 2) {
    config.set_max_concurrency(c);
    constexpr auto N = 20000;
    std::atomic<size_t> cnt = 0;
    std::atomic<size_t> current = 0;
    folly::coro::Baton finish;
    std::vector<std::thread> threads(16);
    for (auto &thread : threads) {
      thread = std::thread([&] {
        for (auto i = 0; i < N / 16; ++i) {
          auto guard = folly::coro::blockingWait(limiter.lock("A"));
          ++current;
          ASSERT_LE(current, c);
          if (++cnt == N) {
            finish.post();
          }
          --current;
        }
      });
    }
    auto result = folly::coro::blockingWait(
        folly::coro::co_awaitTry(folly::coro::timeout([&]() -> CoTask<void> { co_await finish; }(), 5_s)));
    ASSERT_FALSE(result.hasException());
    for (auto &thread : threads) {
      thread.join();
    }
  }
}

}  // namespace
}  // namespace hf3fs::net::test
