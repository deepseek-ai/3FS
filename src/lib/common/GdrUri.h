#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hf3fs::lib {

constexpr size_t kGdrIpcHandleBytes = 64;

struct GdrUri {
  int deviceId = -1;
  size_t size = 0;
  std::array<uint8_t, kGdrIpcHandleBytes> ipcHandle{};
};

std::optional<GdrUri> parseGdrUri(std::string_view uri);

std::string formatGdrUri(int deviceId, size_t size, const uint8_t *ipcHandle, size_t ipcHandleSize);

}  // namespace hf3fs::lib
