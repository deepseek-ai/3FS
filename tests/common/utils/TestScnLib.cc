#include <gtest/gtest.h>
#include <scn/scn.h>
#include <scn/tuple_return.h>

namespace hf3fs::test {
namespace {
TEST(TestScnlib, ScanString) {
  std::string word;
  auto result = scn::scan("Hello world!", "{}", word);

  ASSERT_EQ(word, "Hello");
  ASSERT_EQ(result.range_as_string(), " world!");
}

TEST(TestScnlib, ScanMultipleValues) {
  int i, j;
  auto result = scn::scan("123 456 foo", "{} {}", i, j);

  ASSERT_EQ(i, 123);
  ASSERT_EQ(j, 456);

  std::string str;
  auto ret = scn::scan(result.range(), "{}", str);
  ASSERT_EQ(static_cast<bool>(ret), true);
  ASSERT_EQ(str, "foo");
}

TEST(TestScnlib, ScanTuple) {
  {
    auto [r, i] = scn::scan_tuple<int>("42", "{}");
    ASSERT_EQ(static_cast<bool>(r), true);
    ASSERT_EQ(i, 42);
  }

  {
    auto [r, i, s] = scn::scan_tuple<int, std::string>("42 foo", "{} {}");
    ASSERT_EQ(static_cast<bool>(r), true);
    ASSERT_EQ(i, 42);
    ASSERT_EQ(s, "foo");
  }
}

TEST(TestScnlib, ErrorHandling) {
  int i;
  auto result = scn::scan("foo", "{}", i);
  ASSERT_EQ(static_cast<bool>(result), false);
  ASSERT_EQ(result.range_as_string(), "foo");
}
}  // namespace
}  // namespace hf3fs::test
