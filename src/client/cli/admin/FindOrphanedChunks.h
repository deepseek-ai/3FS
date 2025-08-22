#pragma once

#include "common/utils/Coroutine.h"
#include "common/utils/Path.h"
#include "fbs/storage/Common.h"

namespace hf3fs::client::cli {
class Dispatcher;
CoTryTask<void> registerFindOrphanedChunksHandler(Dispatcher &dispatcher);
}  // namespace hf3fs::client::cli