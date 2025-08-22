#pragma once

#include "common/utils/String.h"

namespace hf3fs::logging {
void initLogHandlers();
bool init(const String &config);
void initOrDie(const String &config);
}  // namespace hf3fs::logging
