#pragma once

#include "common/utils/ConfigBase.h"
#include "common/utils/Duration.h"

namespace hf3fs::kv {

struct TransactionRetry : ConfigBase<TransactionRetry> {
  CONFIG_HOT_UPDATED_ITEM(max_backoff, 1_s);
  CONFIG_HOT_UPDATED_ITEM(max_retry_count, 10u);
};

}  // namespace hf3fs::kv
