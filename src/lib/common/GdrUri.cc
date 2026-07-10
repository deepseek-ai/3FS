#include "lib/common/GdrUri.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <system_error>

namespace hf3fs::lib {
namespace {

constexpr std::string_view kPrefix = "gdr://v2/device/";
constexpr std::string_view kAllocationSep = "/allocation/";
constexpr std::string_view kOffsetSep = "/offset/";
constexpr std::string_view kSizeSep = "/size/";
constexpr std::string_view kIpcSep = "/ipc/";

std::optional<size_t> parseUnsigned(std::string_view text) {
  if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }

  size_t value = 0;
  auto *begin = text.data();
  auto *end = begin + text.size();
  auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool decodeHex(std::string_view encoded, std::array<uint8_t, kGdrIpcHandleBytes> &out) {
  if (encoded.size() != kGdrIpcHandleBytes * 2) {
    return false;
  }

  for (size_t i = 0; i < kGdrIpcHandleBytes; ++i) {
    int hi = hexNibble(encoded[2 * i]);
    int lo = hexNibble(encoded[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

std::string encodeHex(const uint8_t *data, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out[2 * i] = kHex[(data[i] >> 4) & 0x0F];
    out[2 * i + 1] = kHex[data[i] & 0x0F];
  }
  return out;
}

}  // namespace

std::optional<GdrUri> parseGdrUri(std::string_view uri) {
  if (!uri.starts_with(kPrefix)) {
    return std::nullopt;
  }

  uri.remove_prefix(kPrefix.size());
  auto allocationPos = uri.find(kAllocationSep);
  if (allocationPos == std::string_view::npos) {
    return std::nullopt;
  }
  auto deviceText = uri.substr(0, allocationPos);
  uri.remove_prefix(allocationPos + kAllocationSep.size());

  auto offsetPos = uri.find(kOffsetSep);
  if (offsetPos == std::string_view::npos) {
    return std::nullopt;
  }
  auto allocationText = uri.substr(0, offsetPos);
  uri.remove_prefix(offsetPos + kOffsetSep.size());

  auto sizePos = uri.find(kSizeSep);
  if (sizePos == std::string_view::npos) {
    return std::nullopt;
  }
  auto offsetText = uri.substr(0, sizePos);
  uri.remove_prefix(sizePos + kSizeSep.size());

  auto ipcPos = uri.find(kIpcSep);
  if (ipcPos == std::string_view::npos) {
    return std::nullopt;
  }
  auto sizeText = uri.substr(0, ipcPos);
  auto ipcHex = uri.substr(ipcPos + kIpcSep.size());

  auto device = parseUnsigned(deviceText);
  auto allocationSize = parseUnsigned(allocationText);
  auto offset = parseUnsigned(offsetText);
  auto size = parseUnsigned(sizeText);
  if (!device || *device > static_cast<size_t>(std::numeric_limits<int>::max()) || !allocationSize ||
      *allocationSize == 0 || !offset || !size || *size == 0 || *offset > *allocationSize ||
      *size > *allocationSize - *offset) {
    return std::nullopt;
  }

  GdrUri parsed;
  parsed.deviceId = static_cast<int>(*device);
  parsed.allocationSize = *allocationSize;
  parsed.offset = *offset;
  parsed.size = *size;
  if (!decodeHex(ipcHex, parsed.ipcHandle)) {
    return std::nullopt;
  }
  return parsed;
}

std::string formatGdrUri(int deviceId,
                         size_t allocationSize,
                         size_t offset,
                         size_t size,
                         const uint8_t *ipcHandle,
                         size_t ipcHandleSize) {
  if (deviceId < 0 || allocationSize == 0 || size == 0 || offset > allocationSize || size > allocationSize - offset ||
      ipcHandle == nullptr || ipcHandleSize != kGdrIpcHandleBytes) {
    return {};
  }

  auto ipcHex = encodeHex(ipcHandle, ipcHandleSize);
  std::string uri;
  uri.reserve(kPrefix.size() + 20 + kAllocationSep.size() + 20 + kOffsetSep.size() + 20 + kSizeSep.size() + 20 +
              kIpcSep.size() + ipcHex.size());
  uri += kPrefix;
  uri += std::to_string(deviceId);
  uri += kAllocationSep;
  uri += std::to_string(allocationSize);
  uri += kOffsetSep;
  uri += std::to_string(offset);
  uri += kSizeSep;
  uri += std::to_string(size);
  uri += kIpcSep;
  uri += ipcHex;
  return uri;
}

}  // namespace hf3fs::lib
