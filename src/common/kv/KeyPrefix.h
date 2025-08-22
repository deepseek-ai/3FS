#pragma once

#include <cstdint>
#include <string_view>

namespace hf3fs::kv {
// fixed size prefix
inline constexpr uint32_t makePrefixValue(const char (&s)[5]) {
  return s[0] + (static_cast<uint32_t>(s[1]) << 8) + (static_cast<uint32_t>(s[2]) << 16) +
         (static_cast<uint32_t>(s[3]) << 24);
}

// use enum for avoiding duplicated prefixes.
enum class KeyPrefix : uint32_t {
  Unknown = makePrefixValue("UNKW"),
#define DEFINE_PREFIX(name, s) name = makePrefixValue(s),

#include "KeyPrefix-def.h"
};

inline constexpr std::string_view toStr(KeyPrefix prefix) {
  switch (prefix) {
#define DEFINE_PREFIX(name, s) \
  case KeyPrefix::name:        \
    return s;

#include "KeyPrefix-def.h"
    default:
      return "UNKW";
  }
}

}  // namespace hf3fs::kv
