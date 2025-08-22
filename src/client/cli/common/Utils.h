#pragma once

#include "common/utils/Status.h"

#define ENSURE_USAGE(condition, ...)                                                                       \
  do {                                                                                                     \
    if (!UNLIKELY(static_cast<bool>(condition)))                                                           \
      throw hf3fs::StatusException(hf3fs::Status(hf3fs::CliCode::kWrongUsage __VA_OPT__(, ) __VA_ARGS__)); \
  } while (false)
