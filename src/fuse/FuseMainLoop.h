#pragma once

#include "common/utils/String.h"

namespace hf3fs::fuse {
int fuseMainLoop(const String &programName,
                 bool allowOther,
                 const String &mountpoint,
                 size_t maxbufsize,
                 const String &clusterId);
}
