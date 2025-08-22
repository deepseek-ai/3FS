#include <atomic>
#include <chrono>
#include <folly/Random.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/portability/GTest.h>
#include <functional>
#include <thread>

namespace hf3fs::test {
namespace {

struct Dummy {
  std::array<uint8_t, 64u> x;
};

TEST(TestConcurrentHashMap, Normal) {
  constexpr auto N = 1000000;

  std::atomic<bool> stop = false;
  std::atomic<uint64_t> readTimes = 0;
  std::atomic<uint64_t> writeTimes = 0;

  folly::ConcurrentHashMap<uint64_t, Dummy> map;
  for (auto i = 0; i < N; ++i) {
    map.insert_or_assign(i, Dummy{});
  }

  auto start = std::chrono::steady_clock::now();
  std::jthread writer([&] {
    while (!stop) {
      map.insert_or_assign(folly::Random::rand32() % N, Dummy{});
      ++writeTimes;
    }
  });

  std::vector<std::jthread> readers(4);
  for (auto &reader : readers) {
    reader = std::jthread([&] {
      while (!stop) {
        auto it = map.find(folly::Random::rand32() % N);
        ++readTimes;
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop = true;
  writer.join();
  for (auto &reader : readers) {
    reader.join();
  }
  auto elapsed = std::chrono::steady_clock::now() - start;
  auto ratio = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  fmt::print("OPS: {} read, {} write\n", readTimes.load() * 1000 / ratio, writeTimes.load() * 1000 / ratio);
}

}  // namespace
}  // namespace hf3fs::test
