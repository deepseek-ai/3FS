#include <gtest/gtest.h>
#include <limits>

#include "common/serde/Serde.h"
#include "common/utils/Varint32.h"

namespace hf3fs::test {
namespace {

TEST(TestVarint32, Normal) {
  Varint32 value = 20;
  value = 30;
  ASSERT_EQ(value, 30);

  auto out = serde::serialize(value);

  Varint32 de1{};
  ASSERT_TRUE(serde::deserialize(de1, out).hasValue());
  ASSERT_EQ(de1, value);

  Varint64 de2{};
  ASSERT_TRUE(serde::deserialize(de2, out).hasValue());
  ASSERT_EQ(de2, value);
}

TEST(TestVarint64, Normal) {
  for (auto value : {0ul, 1ul << 32, std::numeric_limits<uint64_t>::max()}) {
    Varint64 ser = value;
    auto out = serde::serialize(ser);

    Varint64 der{};
    ASSERT_TRUE(serde::deserialize(der, out).hasValue());
    ASSERT_EQ(ser, der);
  }

  for (auto value = 1000000ul; value <= 2000000ul; ++value) {
    Varint64 ser = value;
    auto out = serde::serialize(ser);

    Varint64 der{};
    ASSERT_TRUE(serde::deserialize(der, out).hasValue());
    ASSERT_EQ(ser, der);
  }
}

}  // namespace
}  // namespace hf3fs::test
