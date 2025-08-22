#include <folly/logging/xlog.h>

#include "common/utils/ConfigBase.h"
#include "common/utils/Duration.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

static_assert(config::IsPrimitive<Duration>, "duration is not primitive");

class Config : public ConfigBase<Config> {
  CONFIG_HOT_UPDATED_ITEM(c0, Duration::from("0s").value());
  CONFIG_HOT_UPDATED_ITEM(c1, 0_ns);
};

TEST(Duration, testDefaultConstruct) {
  Duration config;
  ASSERT_EQ(config.asMs(), std::chrono::milliseconds{0});
}

TEST(Duration, testParse) {
  Duration c0 = Duration::from("100ns").value();
  ASSERT_EQ(c0.toString(), "100ns");
  Duration c1 = Duration::from("2us").value();
  ASSERT_EQ(c1.toString(), "2us");
  Duration c2 = Duration::from("1001ms").value();
  ASSERT_EQ(c2.toString(), "1s 1ms");
  Duration c3 = Duration::from("45s ").value();
  ASSERT_EQ(c3.toString(), "45s");
  Duration c5 = Duration::from("08h").value();
  ASSERT_EQ(c5.toString(), "8h");
  Duration c6 = Duration::from("1day").value();
  ASSERT_EQ(c6.toString(), "1day");
  ASSERT_FALSE(Duration::from("10mon"));
  ASSERT_FALSE(Duration::from(""));
  ASSERT_FALSE(Duration::from("100"));
  ASSERT_FALSE(Duration::from("ns"));
  ASSERT_FALSE(Duration::from("1.1s"));

  ASSERT_ERROR(Duration::from("1ns2ms"), StatusCode::kInvalidConfig);
  ASSERT_ERROR(Duration::from("1ns2ns"), StatusCode::kInvalidConfig);
  ASSERT_ERROR(Duration::from("1ns2k"), StatusCode::kInvalidConfig);
  ASSERT_ERROR(Duration::from("1ms2.1us"), StatusCode::kInvalidConfig);
  ASSERT_ERROR(Duration::from("ms2us"), StatusCode::kInvalidConfig);
  ASSERT_ERROR(Duration::from("1day2sec"), StatusCode::kInvalidConfig);

  ASSERT_EQ(Duration::from("1day 2s")->toString(), "1day 2s");
  ASSERT_EQ(Duration::from("1day 2h 2001 ms")->toString(), "1day 2h 2s 1ms");
}

TEST(Duration, testConfig) {
  Config cfg;
  ASSERT_EQ(cfg.c0(), Duration::from("0s").value());
  ASSERT_EQ(cfg.c0(), cfg.c1());
  cfg.set_c0(Duration::from("10ms").value());
  ASSERT_EQ(cfg.c0(), Duration::from("10ms").value());

  toml::table result = toml::parse(R"(
    c0 = "5s"
    c1 = "10min"
  )");
  ASSERT_TRUE(cfg.update(result));
  ASSERT_EQ(cfg.c0(), Duration::from("5s").value());
  ASSERT_EQ(cfg.c1(), Duration::from("10min").value());

  XLOGF(INFO, "toString: {}", cfg.toString());

  result = toml::parse(cfg.toString());
  Config other;
  ASSERT_TRUE(other.update(result));
  ASSERT_EQ(cfg.c0(), other.c0());
  ASSERT_EQ(cfg.c1(), other.c1());
}
}  // namespace
}  // namespace hf3fs::test
