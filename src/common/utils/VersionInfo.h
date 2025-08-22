#pragma once

#include <cstdint>
#include <string_view>

namespace hf3fs {

struct VersionInfo {
  static std::string_view fullV0();
  static std::string_view full();
  static uint8_t versionMajor();
  static uint8_t versionMinor();
  static uint8_t versionPatch();
  static uint64_t buildTimeInSeconds();
  static uint32_t buildPipelineId();

  static uint32_t commitHashShort();
  static std::string_view commitHashFull();

  static uint32_t gitTag();
  static uint32_t gitTagSeqNum();
  static bool isReleaseBranch();
};

}  // namespace hf3fs
