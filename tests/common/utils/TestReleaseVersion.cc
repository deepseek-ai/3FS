#include "common/app/AppInfo.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

TEST(TestReleaseVersion, testFormat) {
  auto rv = flat::ReleaseVersion::fromVersionInfoV0();
  ASSERT_EQ(fmt::format("Version: v{}", rv), VersionInfo::fullV0());

  rv = flat::ReleaseVersion::fromVersionInfo();
  ASSERT_EQ(fmt::format("Version: v{}", rv), VersionInfo::full());
}

TEST(TestReleaseVersion, testDeserializeFromV0) {
  auto rv = flat::ReleaseVersion::fromV0(0, 1, 4, 0xabcdef12, 1688016296, 54321);
  auto str = serde::serialize(rv);
  auto unpackRes = flat::ReleaseVersion::unpackFrom(str);
  ASSERT_TRUE(unpackRes);
  auto newRv = *unpackRes;
  ASSERT_EQ(newRv.toString(), "0.1.4-54321-20230629-abcdef12");
  auto rv2 = flat::ReleaseVersion::fromVersionInfo();
  ASSERT_TRUE(rv2 > rv);
}

}  // namespace
}  // namespace hf3fs::test
