#pragma once

#include "FuseClients.h"

namespace hf3fs::fuse {
FuseClients &getFuseClientsInstance();
const fuse_lowlevel_ops &getFuseOps();
}  // namespace hf3fs::fuse
