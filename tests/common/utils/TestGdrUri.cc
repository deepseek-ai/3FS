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

}  // namespace

TEST(TestGdrUri, FormatAndParseRoundTrip) {
  auto handle = makeHandle();

  auto uri = formatGdrUri(2, 4096, handle.data(), handle.size());
  ASSERT_FALSE(uri.empty());

  auto parsed = parseGdrUri(uri);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->deviceId, 2);
  EXPECT_EQ(parsed->size, 4096u);
  EXPECT_EQ(parsed->ipcHandle, handle);
}

TEST(TestGdrUri, FormatRejectsInvalidInput) {
  auto handle = makeHandle();

  EXPECT_TRUE(formatGdrUri(-1, 4096, handle.data(), handle.size()).empty());
  EXPECT_TRUE(formatGdrUri(0, 0, handle.data(), handle.size()).empty());
  EXPECT_TRUE(formatGdrUri(0, 4096, nullptr, handle.size()).empty());
  EXPECT_TRUE(formatGdrUri(0, 4096, handle.data(), handle.size() - 1).empty());
}

TEST(TestGdrUri, ParseRejectsMalformedInput) {
  auto handle = makeHandle();
  auto valid = formatGdrUri(1, 8192, handle.data(), handle.size());
  ASSERT_FALSE(valid.empty());

  EXPECT_FALSE(parseGdrUri(""));
  EXPECT_FALSE(parseGdrUri("gdr://v1/device//size/8192/ipc/" + valid.substr(valid.find("/ipc/") + 5)));
  EXPECT_FALSE(parseGdrUri("gdr://v1/device/-1/size/8192/ipc/" + valid.substr(valid.find("/ipc/") + 5)));
  EXPECT_FALSE(parseGdrUri("gdr://v1/device/1/size/0/ipc/" + valid.substr(valid.find("/ipc/") + 5)));
  EXPECT_FALSE(parseGdrUri("gdr://v1/device/1/size/8192/ipc/"));
  EXPECT_FALSE(parseGdrUri(valid + "/tail"));

  auto badHex = valid;
  badHex.back() = 'x';
  EXPECT_FALSE(parseGdrUri(badHex));

  auto shortHex = valid.substr(0, valid.size() - 2);
  EXPECT_FALSE(parseGdrUri(shortHex));
}

TEST(TestGdrUri, ParseRejectsOverflow) {
  auto handle = makeHandle();
  auto valid = formatGdrUri(0, 1, handle.data(), handle.size());
  auto ipcHex = valid.substr(valid.find("/ipc/") + 5);

  auto deviceOverflow = std::to_string(static_cast<uint64_t>(std::numeric_limits<int>::max()) + 1);
  EXPECT_FALSE(parseGdrUri("gdr://v1/device/" + deviceOverflow + "/size/1/ipc/" + ipcHex));

  std::string sizeOverflow = "184467440737095516160";
  EXPECT_FALSE(parseGdrUri("gdr://v1/device/0/size/" + sizeOverflow + "/ipc/" + ipcHex));
}

}  // namespace hf3fs::lib
