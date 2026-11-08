#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <string>

#include "lib/common/GdrUri.h"

namespace hf3fs::lib {
namespace {

std::array<uint8_t, kGdrIpcHandleBytes> makeHandle() {
  std::array<uint8_t, kGdrIpcHandleBytes> handle{};
  for (size_t i = 0; i < handle.size(); ++i) {
    handle[i] = static_cast<uint8_t>(i * 3 + 7);
  }
  return handle;
}

std::string ipcHex(const std::string &uri) {
  auto position = uri.find("/ipc/");
  return uri.substr(position + 5);
}

}  // namespace

TEST(TestGdrUri, FormatAndParseV2RoundTripWithOffset) {
  auto handle = makeHandle();

  auto uri = formatGdrUri(2, 16384, 4096, 8192, handle.data(), handle.size());
  ASSERT_FALSE(uri.empty());
  EXPECT_EQ(uri, "gdr://v2/device/2/allocation/16384/offset/4096/size/8192/ipc/" + ipcHex(uri));

  auto parsed = parseGdrUri(uri);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->deviceId, 2);
  EXPECT_EQ(parsed->allocationSize, 16384u);
  EXPECT_EQ(parsed->offset, 4096u);
  EXPECT_EQ(parsed->size, 8192u);
  EXPECT_EQ(parsed->ipcHandle, handle);
}

TEST(TestGdrUri, FormatRejectsInvalidInputAndBounds) {
  auto handle = makeHandle();

  EXPECT_TRUE(formatGdrUri(-1, 4096, 0, 4096, handle.data(), handle.size()).empty());
  EXPECT_TRUE(formatGdrUri(0, 0, 0, 1, handle.data(), handle.size()).empty());
  EXPECT_TRUE(formatGdrUri(0, 4096, 0, 0, handle.data(), handle.size()).empty());
  EXPECT_TRUE(formatGdrUri(0, 4096, 4097, 1, handle.data(), handle.size()).empty());
  EXPECT_TRUE(formatGdrUri(0, 4096, 4096, 1, handle.data(), handle.size()).empty());
  EXPECT_TRUE(formatGdrUri(0, 4096, 0, 4096, nullptr, handle.size()).empty());
  EXPECT_TRUE(formatGdrUri(0, 4096, 0, 4096, handle.data(), handle.size() - 1).empty());
}

TEST(TestGdrUri, AcceptsMaximumInBoundsViewWithoutOverflow) {
  auto handle = makeHandle();
  constexpr auto max = std::numeric_limits<size_t>::max();

  auto uri = formatGdrUri(std::numeric_limits<int>::max(), max, max - 1, 1, handle.data(), handle.size());
  ASSERT_FALSE(uri.empty());

  auto parsed = parseGdrUri(uri);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->deviceId, std::numeric_limits<int>::max());
  EXPECT_EQ(parsed->allocationSize, max);
  EXPECT_EQ(parsed->offset, max - 1);
  EXPECT_EQ(parsed->size, 1u);
}

TEST(TestGdrUri, ParseRejectsMalformedFieldsAndTrailingData) {
  auto handle = makeHandle();
  auto valid = formatGdrUri(1, 8192, 1024, 4096, handle.data(), handle.size());
  ASSERT_FALSE(valid.empty());
  auto hex = ipcHex(valid);

  EXPECT_FALSE(parseGdrUri(""));
  EXPECT_FALSE(parseGdrUri("gdr://v1/device/1/allocation/8192/offset/0/size/1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device//allocation/8192/offset/0/size/1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/-1/allocation/8192/offset/0/size/1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/+1/allocation/8192/offset/0/size/1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/1/allocation//offset/0/size/1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/1/allocation/-1/offset/0/size/1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/1/allocation/0/offset/0/size/1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/1/allocation/8192/offset//size/1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/1/allocation/8192/offset/-1/size/1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/1/allocation/8192/offset/0/size//ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/1/allocation/8192/offset/0/size/-1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/1/allocation/8192/offset/0/size/0/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/1/allocation/8192/offset/0/size/1/ipc/"));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/1/allocation/8192/offset/0/size/ 1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri(valid + "/tail"));

  auto badHex = valid;
  badHex.back() = 'x';
  EXPECT_FALSE(parseGdrUri(badHex));
  badHex = valid;
  badHex[badHex.find("/ipc/") + 5] = 'g';
  EXPECT_FALSE(parseGdrUri(badHex));
  EXPECT_FALSE(parseGdrUri(valid.substr(0, valid.size() - 2)));
}

TEST(TestGdrUri, ParseRejectsNumericOverflowAndOutOfBoundsViews) {
  auto handle = makeHandle();
  auto valid = formatGdrUri(0, 1, 0, 1, handle.data(), handle.size());
  auto hex = ipcHex(valid);

  auto deviceOverflow = std::to_string(static_cast<uint64_t>(std::numeric_limits<int>::max()) + 1);
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/" + deviceOverflow + "/allocation/1/offset/0/size/1/ipc/" + hex));

  constexpr auto numericOverflow = "184467440737095516160";
  EXPECT_FALSE(
      parseGdrUri(std::string("gdr://v2/device/0/allocation/") + numericOverflow + "/offset/0/size/1/ipc/" + hex));
  EXPECT_FALSE(
      parseGdrUri(std::string("gdr://v2/device/0/allocation/1/offset/") + numericOverflow + "/size/1/ipc/" + hex));
  EXPECT_FALSE(
      parseGdrUri(std::string("gdr://v2/device/0/allocation/1/offset/0/size/") + numericOverflow + "/ipc/" + hex));

  EXPECT_FALSE(parseGdrUri("gdr://v2/device/0/allocation/4096/offset/4097/size/1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/0/allocation/4096/offset/4096/size/1/ipc/" + hex));
  EXPECT_FALSE(parseGdrUri("gdr://v2/device/0/allocation/4096/offset/2048/size/2049/ipc/" + hex));
}

}  // namespace hf3fs::lib
