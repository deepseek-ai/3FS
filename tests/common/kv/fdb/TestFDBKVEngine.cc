#include <gtest/gtest.h>

#include "FDBTestBase.h"
#include "fdb/FDBKVEngine.h"

namespace hf3fs::kv {

class TestFDBKVEngine : public FDBTestBase {};

TEST_F(TestFDBKVEngine, Construct) {
  ASSERT_EQ(db_.error(), 0);
  FDBKVEngine engine(std::move(db_));
  ASSERT_TRUE(engine.createReadonlyTransaction() != nullptr);
  ASSERT_TRUE(engine.createReadWriteTransaction() != nullptr);
}

}  // namespace hf3fs::kv
