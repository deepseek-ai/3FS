#pragma once

#include "common/app/AppInfo.h"

namespace hf3fs {
Result<String> renderConfig(const String &configTemplate,
                            const flat::AppInfo *app,
                            const std::map<String, String> *envs = nullptr);
Result<flat::ReleaseVersion> parseReleaseVersion(const String &str, const flat::ReleaseVersion &actual);
}  // namespace hf3fs
