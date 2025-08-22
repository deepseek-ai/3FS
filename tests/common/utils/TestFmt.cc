#include <fmt/chrono.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gtest/gtest.h>

namespace hf3fs::tests {
namespace {
TEST(Fmt, testFormat) {
  ASSERT_EQ(fmt::format("Hello {}", "world"), "Hello world");
  ASSERT_EQ(fmt::format("{}", std::vector<std::string>{"\naan"}), "[\"\\naan\"]");
  ASSERT_EQ(fmt::format("{:%S}", std::chrono::milliseconds(1234)), "01.234");
}
}  // namespace
}  // namespace hf3fs::tests
