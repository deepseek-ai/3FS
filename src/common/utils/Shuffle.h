#pragma once

#include <cassert>
#include <cstdint>
#include <folly/Random.h>
#include <folly/logging/xlog.h>
#include <glog/logging.h>
#include <numeric>
#include <optional>
#include <random>
#include <vector>

#include "common/utils/Int128.h"

namespace hf3fs {

#if defined __UINT64_TYPE__ && defined __UINT32_TYPE__ && __SIZEOF_INT128__
#else
#error "error"
#endif

#if (defined(USE_STD_SHUFFLE) + defined(USE_GCC10_SHUFFLE) + defined(USE_GCC11_SHUFFLE)) > 1
#error "multiple shuffle method defined"
#endif

#if defined(USE_STD_SHUFFLE)
__attribute__((used)) inline constexpr char HF3FS_SHUFFLE_METHOD[] = "HF3FS_SHUFFLE_METHOD=STD_SHUFFLE";
#elif defined(USE_GCC10_SHUFFLE)
__attribute__((used)) inline constexpr char HF3FS_SHUFFLE_METHOD[] = "HF3FS_SHUFFLE_METHOD=GCC10_SHUFFLE";
#elif defined(USE_GCC11_SHUFFLE)
__attribute__((used)) inline constexpr char HF3FS_SHUFFLE_METHOD[] = "HF3FS_SHUFFLE_METHOD=GCC11_SHUFFLE";
#else
#error "shuffle method not defined"
#endif

#if defined(__GLIBCXX__) && defined(_GLIBCXX_RELEASE)
#else
#error "not libstdc++"
#endif

// Lemire's nearly divisionless algorithm.
// Returns an unbiased random number from __g downscaled to [0,__range)
// using an unsigned type _Wp twice as wide as unsigned type _Up.
// https://github.com/gcc-mirror/gcc/blob/a9c5f33f3a88fb9f6324beced1ccf30a6740f094/libstdc%2B%2B-v3/include/bits/uniform_int_dist.h#L252
inline uint64_t fast_range(std::mt19937_64 &urng, uint64_t range) {
  // reference: Fast Random Integer Generation in an Interval
  // ACM Transactions on Modeling and Computer Simulation 29 (1), 2019
  // https://arxiv.org/abs/1805.10941
  uint128_t product = uint128_t(urng()) * uint128_t(range);
  uint64_t low = uint64_t(product);
  if (low < range) {
    uint64_t threshold = -range % range;
    while (low < threshold) {
      product = uint128_t(urng()) * uint128_t(range);
      low = uint64_t(product);
    }
  }

  return product >> 64;
}

// https://github.com/gcc-mirror/gcc/blob/e10dc8fa17ac633dfeca38cadfe0ba974af7e5a3/libstdc%2B%2B-v3/include/bits/uniform_int_dist.h#L288
template <const bool GCC11>
uint64_t gcc_uniform_int(uint64_t a, uint64_t b, std::mt19937_64 &urng) {
  static_assert(std::is_same_v<std::mt19937_64::result_type, uint64_t>);

  constexpr uint64_t urngrange = std::mt19937_64::max() - std::mt19937_64::min();
  static_assert(std::mt19937_64::min() == 0);
  static_assert(std::mt19937_64::max() == std::numeric_limits<uint64_t>::max());
  static_assert(urngrange == std::numeric_limits<uint64_t>::max());

  const uint64_t urange = b - a;
  assert(urngrange >= urange);

  uint64_t ret;
  if (urngrange > urange) {
    if constexpr (GCC11) {
      // downscaling after g++11
      const uint64_t uerange = urange + 1;  // urange can be zero
      ret = fast_range(urng, uerange);
    } else {
      // downscaling for g++10
      const uint64_t uerange = urange + 1;  // __urange can be zero
      const uint64_t scaling = urngrange / uerange;
      const uint64_t past = uerange * scaling;
      do {
        ret = uint64_t(urng());
      } while (ret >= past);
      ret /= scaling;
    }
  } else {
    ret = urng();
  }

  return ret + a;
}

// https://github.com/gcc-mirror/gcc/blob/e10dc8fa17ac633dfeca38cadfe0ba974af7e5a3/libstdc%2B%2B-v3/include/bits/stl_algo.h#L3646
template <const bool GCC11>
std::pair<uint64_t, uint64_t> gen_two_uniform_ints(uint64_t b0, uint64_t b1, std::mt19937_64 &urng) {
  const uint64_t x = gcc_uniform_int<GCC11>(0, (b0 * b1) - 1, urng);
  return std::make_pair(x / b1, x % b1);
}

// https://github.com/gcc-mirror/gcc/blob/e10dc8fa17ac633dfeca38cadfe0ba974af7e5a3/libstdc%2B%2B-v3/include/bits/stl_algo.h#L3688
template <typename T, const bool GCC11>
void gcc_shuffle(std::vector<T> &vec, uint64_t mt19937_64_seed) {
  if (vec.empty()) return;

  std::mt19937_64 urng(mt19937_64_seed);
  constexpr uint64_t urngrange = urng.max() - urng.min();
  static_assert(urngrange == std::numeric_limits<uint64_t>::max());

  const uint64_t urange = vec.size();
  auto first = vec.begin();
  auto last = vec.end();
  if (urngrange / urange >= urange) {
    auto i = first + 1;

    // Since we know the vector isn't empty, an even number of elements
    // means an uneven number of elements /to swap/, in which case we
    // do the first one up front:
    if ((urange % 2) == 0) {
      std::iter_swap(i++, first + gcc_uniform_int<GCC11>(0, 1, urng));
    }

    // Now we know that last - i is even, so we do the rest in pairs,
    // using a single distribution invocation to produce swap positions
    // for two successive elements at a time:
    while (i != last) {
      const uint64_t swap_range = uint64_t(i - first) + 1;
      const std::pair<uint64_t, uint64_t> pospos = gen_two_uniform_ints<GCC11>(swap_range, swap_range + 1, urng);
      std::iter_swap(i++, first + pospos.first);
      std::iter_swap(i++, first + pospos.second);
    }

    return;
  }

  for (auto i = first + 1; i != last; ++i) {
    std::iter_swap(i, first + gcc_uniform_int<GCC11>(0, i - first, urng));
  }
}

template <typename T>
void gcc11_shuffle(std::vector<T> &vec, uint64_t mt19937_64_seed) {
  gcc_shuffle<T, true>(vec, mt19937_64_seed);
}

template <typename T>
void gcc10_shuffle(std::vector<T> &vec, uint64_t mt19937_64_seed) {
  gcc_shuffle<T, false>(vec, mt19937_64_seed);
}

template <typename T>
void std_shuffle(std::vector<T> &vec, uint64_t mt19937_64_seed) {
  std::mt19937_64 gen(mt19937_64_seed);
  std::shuffle(vec.begin(), vec.end(), gen);
}

template <typename T>
void hf3fs_shuffle(std::vector<T> &vec, uint64_t mt19937_64_seed) {
#if defined(USE_STD_SHUFFLE)
  std_shuffle(vec, mt19937_64_seed);
#elif defined(USE_GCC10_SHUFFLE)
  gcc10_shuffle(vec, mt19937_64_seed);
#elif defined(USE_GCC11_SHUFFLE)
  gcc11_shuffle(vec, mt19937_64_seed);
#else
#error "shuffle method not defined"
#endif
}

inline bool safe_shuffle_seed(uint32_t vec_len, uint64_t mt19937_64_seed) {
  std::vector<uint32_t> vec1(vec_len);
  std::iota(vec1.begin(), vec1.end(), 0);
  std::vector<uint32_t> vec2 = vec1;
  std::vector<uint32_t> vec3 = vec1;
  assert(vec1 == vec2 && vec1 == vec3);

  std_shuffle(vec1, mt19937_64_seed);
  gcc_shuffle<uint32_t, true>(vec2, mt19937_64_seed);
  gcc_shuffle<uint32_t, false>(vec3, mt19937_64_seed);

#if (_GLIBCXX_RELEASE >= 11)
  XLOGF_IF(DFATAL, vec1 != vec2, "vector length {}, shuffle seed {}", vec_len, mt19937_64_seed);
#elif (_GLIBCXX_RELEASE >= 10)
  XLOGF_IF(DFATAL, vec1 != vec3, "vector length {}, shuffle seed {}", vec_len, mt19937_64_seed);
#else
#error "g++ version < 10"
#endif

  return vec1 == vec2 && vec1 == vec3;
}

inline std::optional<uint64_t> find_safe_seed(uint32_t vec_len) {
  for (size_t i = 0; i < 1000; i++) {
    auto seed = folly::Random::rand64();
    if (safe_shuffle_seed(vec_len, seed)) {
      return seed;
    }
  }
  // probability should be very low
  XLOGF(DFATAL, "can't find safe shuffle seed for vec size {}", vec_len);
  return std::nullopt;
}

}  // namespace hf3fs
