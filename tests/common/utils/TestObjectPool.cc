#include <gtest/gtest.h>

#include "common/utils/ObjectPool.h"

namespace hf3fs::test {
namespace {

TEST(TestObjectPool, Normal) {
  constexpr auto X = 123;
  static size_t destructTimes = 0;

  struct A {
    ~A() { ++destructTimes; }
    int x = X;
  };
  A *rawPtr = nullptr;

  {
    auto a = ObjectPool<A>::get();
    ASSERT_NE(a.get(), nullptr);
    ASSERT_EQ(a->x, X);
    a->x = 0;
    rawPtr = a.get();
    ASSERT_EQ(destructTimes, 0);

    a.reset();
    ASSERT_EQ(destructTimes, 1);
  }
  ASSERT_EQ(destructTimes, 1);
  ASSERT_EQ(ObjectPool<A>::get().get(), rawPtr);
  ASSERT_EQ(ObjectPool<A>::get()->x, X);
}

TEST(TestObjectPool, Allocate) {
  constexpr auto N = 1000000;
  static size_t constructTimes = 0;
  static size_t destructTimes = 0;
  struct A {
    A() { ++constructTimes; }
    ~A() { ++destructTimes; }
  };

  {
    std::vector<ObjectPool<A>::Ptr> vec{N};
    for (auto &item : vec) {
      item = ObjectPool<A>::get();
    }
    ASSERT_EQ(constructTimes, N);
    ASSERT_EQ(destructTimes, 0);
  }
  ASSERT_EQ(destructTimes, N);
}

TEST(TestObjectPool, AllocateAndRelease) {
  constexpr auto N = 1000000;
  static size_t constructTimes = 0;
  static size_t destructTimes = 0;
  struct A {
    A() { ++constructTimes; }
    ~A() { ++destructTimes; }
  };

  for (auto i = 0; i < N; ++i) {
    ObjectPool<A>::get();
  }
  ASSERT_EQ(constructTimes, N);
  ASSERT_EQ(destructTimes, N);
}

}  // namespace
}  // namespace hf3fs::test
