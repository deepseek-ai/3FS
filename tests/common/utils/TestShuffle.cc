#include <cstdint>
#include <folly/Random.h>
#include <folly/experimental/TestUtil.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "common/utils/Shuffle.h"
#include "fmt/core.h"

DEFINE_int32(test_shuffle, 10000, "shuffle times");
DEFINE_string(test_dump_path, "", "3fs meta dump path");

namespace hf3fs::test {
namespace {

TEST(TestShuffle, Basic) {
  // most random seed should be safe if vector size is small
  for (int i = 0; i < FLAGS_test_shuffle; i++) {
    auto seed = folly::Random::rand64();
    auto vecsize = folly::Random::rand32(200);
    ASSERT_TRUE(safe_shuffle_seed(vecsize, seed)) << " " << seed << " " << vecsize;
  }
}

TEST(TestShuffle, FindUnsafeSeed) {
  // for large vector size, we can find mismatch seed
  for (int i = 0; i < 10000; i++) {
    auto seed = folly::Random::rand64();
    auto vecsize = 10000;
    if (!safe_shuffle_seed(vecsize, seed)) {
      fmt::println("find unsafe seed {} for vecsize {}", seed, vecsize);
      return;
    }
  }
  ASSERT_TRUE(false);
}

TEST(TestShuffle, KnownUnsafeSeed) {
  // we know some seed is not safe
  std::vector<uint64_t> unsafeSeeds = {6853942027173882469ull,
                                       12595889032228965821ull,
                                       8619779154399616190ull,
                                       12360005796445515182ull,
                                       14687818431154681368ull,
                                       17026414701752999916ull,
                                       12928634241705049562ull,
                                       5961001693986322440ull,
                                       8264875872430788387ull,
                                       8715369310580772031ull,
                                       17199606764515290853ull,
                                       15088078859152357184ull,
                                       13543500868882399137ull,
                                       472380080658457497ull};
  for (auto seed : unsafeSeeds) {
    ASSERT_FALSE(safe_shuffle_seed(200, seed)) << seed;
#if defined(__GLIBCXX__) && defined(_GLIBCXX_RELEASE)
    std::vector<uint32_t> vec1(200);
    std::iota(vec1.begin(), vec1.end(), 0);
    std::vector<uint32_t> vec2 = vec1;
    std_shuffle(vec1, seed);
#if (_GLIBCXX_RELEASE >= 11)
    gcc11_shuffle(vec2, seed);
#elif (_GLIBCXX_RELEASE >= 10)
    gcc10_shuffle(vec2, seed);
#else
    static_assert(false);
#endif
    ASSERT_EQ(vec1, vec2) << "seed: " << seed;
#else
    static_assert(false);
#endif
  }
}

TEST(TestShuffle, CheckDump) {
  if (FLAGS_test_dump_path.empty()) {
    GTEST_SKIP();
  }

  std::ifstream file(FLAGS_test_dump_path);
  ASSERT_TRUE(file.is_open()) << "failed to open: " << FLAGS_test_dump_path;

  std::string line;
  std::getline(file, line); // skip header

  std::cerr << "id,seed,size,match_gcc10,match_gcc11" << std::endl;

  uint64_t total = 0;
  uint64_t mismatch_g10 = 0, mismatch_g11 = 0, mismatch_both = 0;

  while (std::getline(file, line)) {
      std::istringstream iss(line);
      std::string id_str, seed_str, size_str;

      std::getline(iss, id_str, ',');
      std::getline(iss, seed_str, ',');
      std::getline(iss, size_str, ',');

      uint64_t id = std::stoull(id_str, nullptr, 16);
      uint64_t seed = std::stoull(seed_str);
      uint64_t size = std::stoull(size_str);

      std::vector<uint64_t> vec_std(size), vec_g10(size), vec_g11(size);
      std::iota(vec_std.begin(), vec_std.end(), 0);
      std::iota(vec_g10.begin(), vec_g10.end(), 0);
      std::iota(vec_g11.begin(), vec_g11.end(), 0);

      std_shuffle(vec_std, seed);
      gcc10_shuffle(vec_g10, seed);
      gcc11_shuffle(vec_g11, seed);

      bool match_g10 = (vec_std == vec_g10);
      bool match_g11 = (vec_std == vec_g11);

      if (!match_g10 || !match_g11) {
          std::cerr << std::hex << id << std::dec << ","
                << seed << ","
                << size << ","
                << (match_g10 ? "true" : "false") << ","
                << (match_g11 ? "true" : "false") << std::endl;
      }

      if (!match_g10 && !match_g11) {
          mismatch_both++;
      } else if (!match_g10) {
          mismatch_g10++;
      } else if (!match_g11) {
          mismatch_g11++;
      }

      total++;
      if (total % 10000 == 0) {
          std::cout << "Progress: " << total
                    << ", g++10_only_mismatch: " << mismatch_g10
                    << ", g++11_only_mismatch: " << mismatch_g11
                    << ", both_mismatch: " << mismatch_both << std::endl;
      }
  }

  std::cout << "Done. Total: " << total
            << ", g++10_only_mismatch: " << mismatch_g10
            << ", g++11_only_mismatch: " << mismatch_g11
            << ", both_mismatch: " << mismatch_both << std::endl;

}

}  // namespace
}  // namespace hf3fs::test