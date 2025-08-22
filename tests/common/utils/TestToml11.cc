#include "tests/GtestHelpers.h"
#include "toml.hpp"

namespace hf3fs::test {
namespace {

TEST(TestToml11, Normal) {
  toml::value v;  // not initialized as a table.
  v["foo"] = 42;  // OK. `v` will be a table.
  ASSERT_TRUE(v.is_table());
}

}  // namespace
}  // namespace hf3fs::test
