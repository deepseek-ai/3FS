#include "common/serde/BigEndian.h"
#include "common/serde/Serde.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::serde::test {
namespace {

TEST(TestSerde, BigEndian) {
  {
    using BigSize = BigEndian<std::size_t>;
    BigSize size{0x123456789ABCDEF0ul};
    auto out = serde::serialize(size);
    ASSERT_EQ(out.size(), sizeof(std::size_t));
    ASSERT_EQ(out.front(), 0x12);
    ASSERT_EQ(out.back(), (char)0xF0);
    ASSERT_EQ(serde::toJsonString(size), std::to_string(size));

    ASSERT_OK(serde::fromJsonString(size, "233"));
    ASSERT_EQ(size, 233);
  }
}

}  // namespace
}  // namespace hf3fs::serde::test
