#include <stdexcept>
#include <unordered_map>

#include "common/utils/RobinHood.h"
#include "common/utils/UnorderedDense.h"
#include "tests/GtestHelpers.h"

namespace {

template <class Map>
void insertWhileHashIsZero() {
  constexpr auto N = 1000;
  Map map;
  for (auto i = 0; i < N; ++i) {
    map[i] = i;
  }
}

TEST(TestUnorderedDense, Normal) {
  struct Hash {
    size_t operator()(int) const { return 0; }
  };

  insertWhileHashIsZero<ankerl::unordered_dense::map<int, int, Hash>>();
  insertWhileHashIsZero<std::unordered_map<int, int, Hash>>();
  ASSERT_THROW((insertWhileHashIsZero<robin_hood::unordered_map<int, int, Hash>>()), std::overflow_error);
}

}  // namespace
