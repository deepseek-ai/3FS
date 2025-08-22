#include <gtest/gtest.h>

#include "common/utils/FdWrapper.h"

namespace hf3fs::test {
namespace {

TEST(TestFdWrapper, Normal) {
  int p[2];
  ASSERT_EQ(::pipe(p), 0);

  FdWrapper fd;
  ASSERT_EQ(fd, -1);

  fd = p[0];
  fd = FdWrapper{p[1]};  // p[0] is closed.
  ASSERT_EQ(fd, p[1]);

  fd = FdWrapper{};  // p[1] is closed.
  ASSERT_EQ(fd, -1);
}

}  // namespace
}  // namespace hf3fs::test
