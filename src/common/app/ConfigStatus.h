#pragma once

#include <cstdint>

namespace hf3fs {
enum class ConfigStatus : uint8_t { NORMAL = 0, DIRTY = 1, FAILED = 2, UNKNOWN = 3, STALE = 4 };
}
