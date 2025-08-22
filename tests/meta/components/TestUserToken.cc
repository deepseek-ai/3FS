#include <folly/Random.h>

#include "core/user/UserToken.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::meta::server::test {
namespace {
class UserTokenTest : public ::testing::Test {
 protected:
  void test(uint32_t uid, uint64_t randomv) {
    auto token = core::encodeUserToken(uid, randomv);
    XLOGF(DBG, "encode({}, {}) -> {}", uid, randomv, token);

    auto decodeRes = core::decodeUserToken(token);
    ASSERT_OK(decodeRes);

    auto [duid, dts] = *decodeRes;
    ASSERT_EQ(uid, duid);
    ASSERT_EQ(randomv, dts);

    auto decodeRes2 = core::decodeUidFromUserToken(token);
    ASSERT_OK(decodeRes2);
    ASSERT_EQ(uid, *decodeRes2);
  }
};

TEST_F(UserTokenTest, test) {
  for (uint32_t i = 0; i < 100; ++i) {
    for (uint64_t j = 0; j < 100; ++j) {
      test(i, j);
    }
  }
  for (size_t i = 0; i < 10000; ++i) {
    auto uid = folly::Random::rand32();
    auto randomv = folly::Random::rand64();
    test(uid, randomv);
  }
}
}  // namespace
}  // namespace hf3fs::meta::server::test
