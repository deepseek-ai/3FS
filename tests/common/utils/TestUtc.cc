#include <gtest/gtest.h>

#include "common/utils/UtcTime.h"

namespace hf3fs::tests {
namespace {

TEST(UtcTime, testConvert) {
  UtcTime now = UtcClock::now();
  int64_t us = now.toMicroseconds();
  UtcTime utc = UtcTime::fromMicroseconds(us);

  ASSERT_EQ(us, utc.toMicroseconds());
}

}  // namespace
}  // namespace hf3fs::tests
