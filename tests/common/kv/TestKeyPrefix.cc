#include <gtest/gtest.h>
#include <string>

#include "common/kv/KeyPrefix.h"
#include "fdb/FDB.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::tests {
namespace {
TEST(KeyPrefixTest, test) {
  ASSERT_EQ(static_cast<uint32_t>(kv::KeyPrefix::Inode), kv::makePrefixValue("INOD"));
  ASSERT_EQ(kv::toStr(kv::KeyPrefix::Inode), "INOD");
  ASSERT_EQ(kv::toStr(static_cast<kv::KeyPrefix>(-1)), "UNKW");
}

TEST(PrefixListEndKey, test) {
  ASSERT_EQ(kv::TransactionHelper::prefixListEndKey("a"), std::string("b"));
  ASSERT_EQ(kv::TransactionHelper::prefixListEndKey("a1"), std::string("a2"));
  ASSERT_EQ(kv::TransactionHelper::prefixListEndKey("a\xff"), std::string("b"));
  ASSERT_EQ(kv::TransactionHelper::prefixListEndKey("a\xff\xff"), std::string("b"));
  ASSERT_EQ(kv::TransactionHelper::prefixListEndKey("a\xff\xff\xff"), std::string("b"));
  ASSERT_EQ(kv::TransactionHelper::prefixListEndKey("\xff"
                                                    "a\xff\xff"),
            std::string("\xff"
                        "b"));
  ASSERT_EQ(kv::TransactionHelper::prefixListEndKey("\xff\xff"), std::string(""));
}
}  // namespace
}  // namespace hf3fs::tests
