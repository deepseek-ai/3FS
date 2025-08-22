#pragma once

#include <cassert>
#include <folly/Random.h>
#include <span>
#include <vector>

namespace hf3fs {

struct RandomUtils {
  template <typename T>
  static T randomSelect(const std::vector<T> &vec) {
    assert(!vec.empty());
    return vec[folly::Random::rand64(vec.size())];
  }
};

}  // namespace hf3fs