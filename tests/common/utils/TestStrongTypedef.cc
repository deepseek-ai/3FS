#include <string>

#include "common/utils/StrongType.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::tests {
namespace {
STRONG_TYPEDEF(int, TypedInt);
STRONG_TYPEDEF(size_t, TypedUint);
STRONG_TYPEDEF(std::string, TypedString);

TEST(StrongTypedefTest, testComparison) {
  TypedInt ti0;
  ASSERT_TRUE(ti0 < 1);
  ASSERT_TRUE(ti0 > -1);
  ASSERT_EQ(ti0, 0);

  TypedUint tu0;
  ASSERT_TRUE(tu0.toUnderType() < 1);
  ASSERT_TRUE(tu0.toUnderType() >= 0);
  ASSERT_EQ(tu0, 0);

  TypedString ts0, ts1("aa");
  ASSERT_TRUE(ts0 < ts1);
  ASSERT_TRUE(ts0 < "a");
}
}  // namespace
}  // namespace hf3fs::tests
