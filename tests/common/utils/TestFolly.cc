#include <folly/hash/Checksum.h>
#include <folly/logging/xlog.h>

#include "tests/GtestHelpers.h"

namespace hf3fs::tests {
namespace {

TEST(Folly, CRC32C) {
  std::string a = "hello";
  auto crc1 = folly::crc32c(reinterpret_cast<const uint8_t *>(a.data()), a.size(), 0);

  std::string b = "world";
  auto crc2 = folly::crc32c(reinterpret_cast<const uint8_t *>(b.data()), b.size(), 0);

  auto x = folly::crc32c_combine(crc1, crc2, b.size());
  auto y = folly::crc32c(reinterpret_cast<const uint8_t *>(b.data()), b.size(), crc1);
  ASSERT_EQ(x, y);

  auto crc32c1 = ~0x14298C12;  // 1MB zero.
  auto crc32c2 = ~0x527D5351;  // one zero.
  auto out = folly::crc32c_combine(~crc32c1, crc32c2, 1);
  XLOGF(INFO, "{:08X} {:08X}", out, ~out);
}

}  // namespace
}  // namespace hf3fs::tests
