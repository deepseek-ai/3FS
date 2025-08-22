#pragma once

#include "common/app/AppInfo.h"

namespace hf3fs::core::launcher {
flat::AppInfo buildBasicAppInfo(flat::NodeId nodeId, const String &clusterId);
}  // namespace hf3fs::core::launcher
