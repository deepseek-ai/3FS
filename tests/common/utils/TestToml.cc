#include <gtest/gtest.h>

#include "common/utils/Toml.hpp"

TEST(TestToml, Normal) {
  toml::parse_result config = toml::parse(R"(
    name = "foo"

    [test]
    foo = "bar"
    val = 123
  )");

  ASSERT_TRUE(config["name"].is_string());
  ASSERT_EQ(config["name"].value_or(std::string_view{}), std::string_view("foo"));

  ASSERT_TRUE(config["test"].is_table());
  ASSERT_EQ(config["test"]["foo"].value<std::string_view>(), "bar");
  ASSERT_EQ(config["test"]["val"].value<int>(), 123);

  ASSERT_FALSE(config.contains("not-exists"));
  ASSERT_FALSE(config["not-exists"].is_table());
  ASSERT_FALSE(config["not-exists"]["not-exists"].is_value());
  ASSERT_EQ(config["not-exists"]["not-exists"].value_or(123), 123);
}
