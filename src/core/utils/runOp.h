#pragma once

#include "common/utils/UtcTime.h"

#define CO_INVOKE_OP(NORMAL_PATH_LOG_LEVEL, op, peer, ...)                          \
  LOG_OP_##NORMAL_PATH_LOG_LEVEL(op, "start execution. from:{}", peer);             \
  auto guard = op.startRecord();                                                    \
  auto result = co_await op.handle(__VA_ARGS__);                                    \
  if (result.hasError()) {                                                          \
    guard.reportWithCode(result.error().code());                                    \
    LOG_OP_ERR(op, "failed: {}. latency: {}", result.error(), *guard.latency());    \
    CO_RETURN_ERROR(result);                                                        \
  } else {                                                                          \
    guard.reportWithCode(StatusCode::kOK);                                          \
    LOG_OP_##NORMAL_PATH_LOG_LEVEL(op, "succeeded. latency: {}", *guard.latency()); \
    co_return result;                                                               \
  }

#define CO_INVOKE_OP_INFO(...) CO_INVOKE_OP(INFO, __VA_ARGS__)
#define CO_INVOKE_OP_DBG(...) CO_INVOKE_OP(DBG, __VA_ARGS__)
