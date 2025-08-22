#include "common/utils/StatusCode.h"
#include "gtest/gtest.h"

namespace hf3fs {
namespace {

TEST(StatusCode, toString) {
  ASSERT_EQ(StatusCode::toString(StatusCode::kOK), "OK");
  ASSERT_EQ(StatusCode::toString(TransactionCode::kConflict), "Transaction::Conflict");
  ASSERT_EQ(StatusCode::toString(-1), "UnknownStatusCode");
}

}  // namespace
}  // namespace hf3fs
