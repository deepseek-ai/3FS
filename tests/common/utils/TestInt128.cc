#include <fmt/format.h>
#include <limits>
#include <set>

#include "common/utils/Int128.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

TEST(TestInt128, Normal) {
  auto value = std::numeric_limits<uint128_t>::max();
  ASSERT_EQ(fmt::format("{}", value), "340282366920938463463374607431768211455");

  std::set<uint128_t> set;
  set.insert(value);
  ASSERT_EQ(set.size(), 1ul);
  ASSERT_TRUE(set.count(value));
  ASSERT_FALSE(set.count(0));
}

}  // namespace
}  // namespace hf3fs::test
