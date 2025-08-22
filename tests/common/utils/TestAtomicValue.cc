#include <folly/experimental/coro/GtestHelpers.h>

#include "common/utils/AtomicValue.h"

namespace hf3fs::test {
namespace {

using namespace std::chrono_literals;

TEST(TestAtomicValue, Normal) {
  AtomicValue<uint64_t> a;
  ASSERT_EQ(a, 0);

  a = 1;
  ASSERT_EQ(a, 1);

  AtomicValue<uint64_t> b = a;
  ASSERT_EQ(b, 1);
}

}  // namespace
}  // namespace hf3fs::test
