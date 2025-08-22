#include "common/utils/Size.h"
#include "common/utils/ZSTD.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::testing {
namespace {

TEST(TestZstd, Normal) {
  std::string src(1_MB, 0xFF);
  std::string out(1_MB, '\0');

  auto compressed = ZSTD_compress(out.data(), out.size(), src.data(), src.size(), 0);
  ASSERT_FALSE(ZSTD_isError(compressed));
  ASSERT_LT(compressed, src.size());
  out.resize(compressed);
  XLOGF(INFO, "zstd compress {} into {}", src.size(), out.size());

  std::string des(1_MB, '\0');
  auto decompressed = ZSTD_decompress(des.data(), des.size(), out.data(), out.size());
  ASSERT_EQ(src.size(), decompressed);
  ASSERT_TRUE(src == des);
}

}  // namespace
}  // namespace hf3fs::testing
