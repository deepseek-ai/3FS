#pragma once

#include <gtest/gtest.h>
#include <memory>
#include <thread>

#include "fdb/FDB.h"
#include "fdb/FDBContext.h"
#include "gtest/gtest.h"
#include "tests/fdb/SetupFDB.h"

namespace hf3fs::kv {
class FDBTestBase : public testing::SetupFDB {
 protected:
  void SetUp() override {
    auto cluster = std::getenv("FDB_UNITTEST_CLUSTER");
    if (!cluster) {
      GTEST_SKIP_("FDB_UNITTEST_CLUSTER not set skip test");
    }
    db_ = fdb::DB(cluster, false);
  }

 protected:
  fdb::DB db_;
};

}  // namespace hf3fs::kv
