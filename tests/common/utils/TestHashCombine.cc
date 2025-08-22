#include <cstdint>
#include <folly/hash/Hash.h>

#include "common/utils/RobinHoodUtils.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {
TEST(HashCombine, testUint64) {
  for (uint64_t x = 0; x < 64; ++x) {
    auto h = folly::hash::hash_combine_generic(RobinHoodHasher{}, x);
    fmt::print("x = {} h = {} h % 256 = {}\n", x, h, h % 256);
  }
}
}  // namespace
}  // namespace hf3fs::test
