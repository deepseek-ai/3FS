#include <gtest/gtest.h>

#include "common/utils/LruCache.h"

namespace hf3fs::test {
namespace {

TEST(TestLruCache, Normal) {
  LruCache<int, int> cache(4);
  ASSERT_TRUE(cache.empty());
  ASSERT_EQ(cache.size(), 0);
  ASSERT_EQ(cache.begin(), cache.end());

  cache[1] = 1;
  cache[2] = 2;
  cache[3] = 3;
  cache[4] = 4;
  ASSERT_FALSE(cache.empty());
  ASSERT_EQ(cache.size(), 4);
  ASSERT_NE(cache.begin(), cache.end());

  auto it = cache.begin();
  ASSERT_EQ(it->second, 4);
  ++it;
  ASSERT_EQ(it->second, 3);
  ++it;
  ASSERT_EQ(it->second, 2);
  ++it;
  ASSERT_EQ(it->second, 1);
  ++it;
  ASSERT_EQ(it, cache.end());

  cache[1] = 1;
  ASSERT_EQ(cache.front().second, 1);
  ASSERT_EQ(cache.back().second, 2);

  ASSERT_TRUE(cache.emplace(5, 5).second);
  ASSERT_EQ(cache.front().second, 5);
  ASSERT_EQ(cache.back().second, 3);
  ASSERT_EQ(cache.size(), 4);

  cache.promote(cache.find(1));
  ASSERT_EQ(cache.front().second, 1);
  ASSERT_EQ(cache.back().second, 3);

  cache.obsolete(cache.find(1));
  ASSERT_EQ(cache.front().second, 5);
  ASSERT_EQ(cache.back().second, 1);

  it = cache.erase(5);
  ASSERT_EQ(it, cache.begin());
  it = cache.erase(cache.find(1));
  ASSERT_EQ(it, cache.end());

  ASSERT_EQ(cache.size(), 2);
  auto idx = 0;
  for (auto &pair : cache) {
    ASSERT_EQ(pair.first, 4 - idx++);
  }
}

TEST(TestLruCache, ManuallyRemove) {
  constexpr auto N = 4;
  constexpr auto M = 8;

  LruCache<int, int> cache(N, false);
  for (auto i = 0; i < M; ++i) {
    cache[i] = i;
  }
  ASSERT_EQ(cache.size(), M);

  cache.evictObsoletedIf([](int, int value) { return value < 2; });
  ASSERT_EQ(cache.size(), 6);
  ASSERT_EQ(cache.back().second, 2);

  cache.evictObsoleted();
  ASSERT_EQ(cache.size(), N);
  ASSERT_EQ(cache.back().second, 4);
}

}  // namespace
}  // namespace hf3fs::test
