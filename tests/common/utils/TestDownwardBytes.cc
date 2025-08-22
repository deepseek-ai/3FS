#include "common/net/Allocator.h"
#include "common/utils/DownwardBytes.h"
#include "common/utils/Size.h"
#include "tests/GtestHelpers.h"

namespace hf3fs {
namespace {

std::vector<std::pair<size_t, bool>> logger;

struct Allocator {
 public:
  static constexpr size_t kDefaultSize = 1_MB;
  static std::uint8_t *allocate(size_t size) {
    size = std::max(size, kDefaultSize);
    logger.emplace_back(size, true);
    return new uint8_t[size];
  }

  static void deallocate(const uint8_t *buf, size_t size) {
    size = std::max(size, kDefaultSize);
    logger.emplace_back(size, false);
    delete[] buf;
  }
};

TEST(TestDownwardBytes, Normal) {
  ASSERT_EQ(logger.size(), 0);

  {
    DownwardBytes<Allocator> bytes;
    ASSERT_EQ(bytes.capacity(), 1_MB);
    ASSERT_EQ(logger.size(), 1);
    ASSERT_EQ(logger.back().first, 1_MB);
    ASSERT_TRUE(logger.back().second);
  }
  ASSERT_EQ(logger.size(), 2);
  ASSERT_EQ(logger.back().first, 1_MB);
  ASSERT_FALSE(logger.back().second);

  {
    DownwardBytes<Allocator> bytes;
    auto old = bytes.data();
    ASSERT_EQ(bytes.capacity(), 1_MB);
    ASSERT_EQ(logger.size(), 3);
    ASSERT_EQ(logger.back().first, 1_MB);
    ASSERT_TRUE(logger.back().second);

    std::vector<uint8_t> empty(512_KB, 'A');
    bytes.append(empty.data(), empty.size());
    ASSERT_EQ(bytes.data() + 512_KB, old);
    ASSERT_EQ(bytes.data()[0], 'A');
    ASSERT_EQ(bytes.data()[512_KB - 1], 'A');
    ASSERT_EQ(bytes.capacity(), 1_MB);
    ASSERT_EQ(bytes.size(), 512_KB);
    ASSERT_EQ(logger.size(), 3);

    std::fill(empty.begin(), empty.end(), 'B');
    bytes.append(empty.data(), empty.size());
    ASSERT_EQ(bytes.data() + 1_MB, old);
    ASSERT_EQ(bytes.data()[0], 'B');
    ASSERT_EQ(bytes.data()[512_KB - 1], 'B');
    ASSERT_EQ(bytes.data()[512_KB], 'A');
    ASSERT_EQ(bytes.data()[1_MB - 1], 'A');
    ASSERT_EQ(bytes.capacity(), 1_MB);
    ASSERT_EQ(bytes.size(), 1_MB);
    ASSERT_EQ(logger.size(), 3);

    std::fill(empty.begin(), empty.end(), 'C');
    bytes.append(empty.data(), 1);
    ASSERT_NE(bytes.data() + 1_MB + 1, old);
    ASSERT_EQ(bytes.data()[0], 'C');
    ASSERT_EQ(bytes.data()[1], 'B');
    ASSERT_EQ(bytes.data()[512_KB], 'B');
    ASSERT_EQ(bytes.data()[512_KB + 1], 'A');
    ASSERT_EQ(bytes.data()[1_MB], 'A');
    ASSERT_EQ(bytes.capacity(), 2_MB);
    ASSERT_EQ(bytes.size(), 1_MB + 1);
    ASSERT_EQ(logger.size(), 5);
    ASSERT_EQ(logger[3].first, 2_MB);
    ASSERT_TRUE(logger[3].second);
    ASSERT_EQ(logger[4].first, 1_MB);
    ASSERT_FALSE(logger[4].second);

    uint32_t offset, capacity;
    auto buf = bytes.release(offset, capacity);
    ASSERT_EQ(logger.size(), 5);
    ASSERT_EQ(bytes.data(), nullptr);
    ASSERT_EQ(bytes.size(), 0);
    ASSERT_EQ(bytes.capacity(), 0);
    ASSERT_EQ(offset, 1_MB - 1);
    ASSERT_EQ(capacity, 2_MB);
    ASSERT_EQ(buf[offset], 'C');
    Allocator::deallocate(buf, capacity);
  }
  ASSERT_EQ(logger.size(), 6);
  ASSERT_EQ(logger.back().first, 2_MB);
  ASSERT_FALSE(logger.back().second);

  {
    DownwardBytes<Allocator> bytes;
    ASSERT_EQ(bytes.capacity(), 1_MB);
    ASSERT_EQ(logger.size(), 7);
    ASSERT_EQ(logger.back().first, 1_MB);
    ASSERT_TRUE(logger.back().second);

    std::vector<uint8_t> empty(2_MB, 'A');
    bytes.append(empty.data(), empty.size());
    ASSERT_EQ(logger.size(), 9);
    ASSERT_EQ(logger.back().first, 1_MB);
    ASSERT_FALSE(logger.back().second);
    ASSERT_EQ(logger[7].first, 2_MB);
    ASSERT_TRUE(logger[7].second);
  }
  ASSERT_EQ(logger.size(), 10);
  ASSERT_EQ(logger.back().first, 2_MB);
  ASSERT_FALSE(logger.back().second);
}

}  // namespace
}  // namespace hf3fs
