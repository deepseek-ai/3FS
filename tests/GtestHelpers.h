#pragma once

#include <folly/experimental/TestUtil.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <gtest/gtest.h>

#define ASSERT_OK(code_to_run)                          \
  do {                                                  \
    auto &&_status = code_to_run;                       \
    ASSERT_TRUE(_status) << _status.error().describe(); \
  } while (0)

#define ASSERT_RESULT_EQ(result, expr) \
  do {                                 \
    auto &&_res = (expr);              \
    ASSERT_OK(_res);                   \
    ASSERT_EQ((result), *_res);        \
  } while (false)

#define ASSERT_ERROR(code_to_run, expected_code)                                    \
  do {                                                                              \
    auto &&_status = code_to_run;                                                   \
    ASSERT_TRUE(_status.hasError());                                                \
    ASSERT_EQ(_status.error().code(), expected_code) << _status.error().describe(); \
  } while (0)

#define CO_ASSERT_OK(code_to_run)                                      \
  do {                                                                 \
    auto &&_status = code_to_run;                                      \
    CO_ASSERT_FALSE(_status.hasError()) << _status.error().describe(); \
  } while (0)

#define CO_ASSERT_ERROR(code_to_run, expected_code)                                    \
  do {                                                                                 \
    auto &&_status = code_to_run;                                                      \
    CO_ASSERT_TRUE(_status.hasError());                                                \
    CO_ASSERT_EQ(_status.error().code(), expected_code) << _status.error().describe(); \
  } while (0)

#define CO_AWAIT_ASSERT_ERROR(error_code, command)                        \
  do {                                                                    \
    auto _r = co_await (command);                                         \
    CO_ASSERT_TRUE(_r.hasError());                                        \
    CO_ASSERT_EQ(_r.error().code(), error_code) << _r.error().describe(); \
  } while (0)

#define CO_AWAIT_ASSERT_OK(command)                          \
  do {                                                       \
    auto _r = co_await (command);                            \
    CO_ASSERT_FALSE(_r.hasError()) << _r.error().describe(); \
  } while (0)
