#include <folly/logging/xlog.h>

#include "common/utils/ConfigBase.h"
#include "common/utils/Size.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

static_assert(config::IsPrimitive<Size>, "size is not primitive");

class Config : public ConfigBase<Config> {
  CONFIG_HOT_UPDATED_ITEM(size, 128_KB);
};

TEST(TestSize, Normal) {
  Size config;
  ASSERT_EQ(size_t(config), 0);

  ASSERT_EQ(1024_B, 1_KB);
  ASSERT_EQ(4096_B, 4_KB);
  ASSERT_EQ(1048576_B, 1_MB);
  ASSERT_EQ(1024_KB, 1_MB);
  ASSERT_EQ(1024_MB, 1_GB);
  ASSERT_EQ(1024_GB, 1_TB);

  ASSERT_EQ(1000_B, 1_K);
  ASSERT_EQ(1000_K, 1_M);
  ASSERT_EQ(1000_M, 1_G);
  ASSERT_EQ(1000_G, 1_T);

  ASSERT_EQ(Size::toString(0), "0");
  ASSERT_EQ(Size::toString(511), "511");
  ASSERT_EQ(Size::toString(512), "512");
  ASSERT_EQ(Size::toString(1013), "1013");
  ASSERT_EQ(Size::toString(1025), "1025");
  ASSERT_EQ(Size::toString(1034), "1034");
  ASSERT_EQ(Size::toString(1048576), "1MB");

  ASSERT_EQ(Size::toString(1000), "1K");
  ASSERT_EQ(Size::toString(1024), "1KB");
  ASSERT_EQ(Size::toString(1000_K), "1M");
  ASSERT_EQ(Size::toString(1024_K), "1024K");
  ASSERT_EQ(Size::toString(1000_G), "1T");
  ASSERT_EQ(Size::toString(1024_G), "1024G");

  ASSERT_EQ(Size::from("1K").value(), 1_K);
  ASSERT_EQ(Size::from("2KB").value(), 2_KB);
  ASSERT_EQ(Size::from("3M").value(), 3_M);
  ASSERT_EQ(Size::from("4MB").value(), 4_MB);
  ASSERT_EQ(Size::from("5G").value(), 5_G);
  ASSERT_EQ(Size::from("6GB").value(), 6_GB);
  ASSERT_EQ(Size::from("7T").value(), 7_T);
  ASSERT_EQ(Size::from("8TB").value(), 8_TB);

  ASSERT_EQ(Size::around(0), "0");
  ASSERT_EQ(Size::around(511), "511");
  ASSERT_EQ(Size::around(512), "0.50KB");
  ASSERT_EQ(Size::around(1013), "0.99KB");
  ASSERT_EQ(Size::around(1025), "1.00KB");
  ASSERT_EQ(Size::around(1034), "1.01KB");
  ASSERT_EQ(Size::around(1048576), "1.00MB");
}

TEST(TestSize, Parse) {
  Size c0 = Size::from("100").value();
  ASSERT_EQ(c0.toString(), "100");
  Size c1 = Size::from("2KB").value();
  ASSERT_EQ(c1.toString(), "2KB");
  Size c2 = Size::from("1001MB").value();
  ASSERT_EQ(c2.toString(), "1001MB");
  Size c3 = Size::from("45").value();
  ASSERT_EQ(c3.toString(), "45");
  Size c5 = Size::from("08GB").value();
  ASSERT_EQ(c5.toString(), "8GB");
  Size c6 = Size::from("1TB").value();
  ASSERT_EQ(c6.toString(), "1TB");
  ASSERT_FALSE(Size::from("10kb"));
  ASSERT_FALSE(Size::from(""));
  ASSERT_FALSE(Size::from("GB"));
  ASSERT_FALSE(Size::from("1.1MB"));

  ASSERT_EQ(fmt::format("{}", 100_KB), "100KB");
}

TEST(TestSize, testConfig) {
  Config cfg;
  ASSERT_EQ(cfg.size(), 128_KB);
  cfg.set_size(512_KB);
  ASSERT_EQ(cfg.size(), 512_KB);

  toml::table result = toml::parse(R"(
    size = "5KB"
  )");
  ASSERT_TRUE(cfg.update(result));
  ASSERT_EQ(cfg.size(), 5_KB);

  XLOGF(INFO, "toString: {}", cfg.toString());

  result = toml::parse(cfg.toString());
  Config other;
  ASSERT_TRUE(other.update(result));
  ASSERT_EQ(cfg.size(), other.size());

  ASSERT_TRUE(cfg.update(toml::parse(R"(
    size = 4096
  )")));
  ASSERT_EQ(cfg.size(), 4_KB);
}

TEST(TestSize, Infinity) {
  constexpr auto inf = Size::infinity();
  ASSERT_EQ(inf, std::numeric_limits<uint64_t>::max());
  ASSERT_EQ(inf.toString(), "infinity");

  ASSERT_OK(Size::from("infinity"));
  ASSERT_EQ(*Size::from("infinity"), Size::infinity());
}

}  // namespace
}  // namespace hf3fs::test
