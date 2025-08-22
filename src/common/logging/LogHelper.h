#pragma once

#include <folly/logging/xlog.h>

namespace hf3fs::logging {

// set logging level to ERR if the specified condition is evaluated to be true
#define ERRLOGF_IF(level, cond, fmt, ...)                     \
  do {                                                        \
    XLOGF_IF(ERR, cond, fmt __VA_OPT__(, ) __VA_ARGS__);      \
    XLOGF_IF(level, !(cond), fmt __VA_OPT__(, ) __VA_ARGS__); \
  } while (false)

// set logging level to WARN if the specified condition is evaluated to be true
#define WARNLOGF_IF(level, cond, fmt, ...)                    \
  do {                                                        \
    XLOGF_IF(WARN, cond, fmt __VA_OPT__(, ) __VA_ARGS__);     \
    XLOGF_IF(level, !(cond), fmt __VA_OPT__(, ) __VA_ARGS__); \
  } while (false)

}  // namespace hf3fs::logging
