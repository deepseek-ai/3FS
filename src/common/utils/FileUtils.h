#pragma once

#include <string>

#include "common/utils/Path.h"
#include "common/utils/Result.h"

namespace hf3fs {
Result<std::string> loadFile(const Path &path);

Result<Void> storeToFile(const Path &path, const std::string &content);

}  // namespace hf3fs
