#pragma once

#include "common/utils/Coroutine.h"

namespace hf3fs::client::cli {
class Dispatcher;
CoTryTask<void> registerStatHandler(Dispatcher &dispatcher);
}  // namespace hf3fs::client::cli
