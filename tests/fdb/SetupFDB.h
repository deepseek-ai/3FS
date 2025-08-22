#pragma once

#include <atomic>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <utility>

#include "fdb/FDB.h"
#include "fdb/FDBConfig.h"
#include "fdb/FDBContext.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::testing {
using namespace hf3fs::kv;

class SetupFDB : public ::testing::Test {
 public:
  static void SetUpTestSuite() {
    static std::once_flag flag;
    std::call_once(flag, [] {
      static auto context = fdb::FDBContext::create({});
      return;
    });
  }
};

}  // namespace hf3fs::testing
