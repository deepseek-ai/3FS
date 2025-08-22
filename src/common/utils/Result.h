#pragma once

#include <cassert>
#include <folly/Expected.h>
#include <folly/logging/xlog.h>

#include "common/utils/Status.h"

#define RETURN_ERROR(result) return hf3fs::makeError(std::move(result.error()))

#define MAKE_ERROR_F(code, ...) hf3fs::makeError((code), fmt::format(__VA_ARGS__))

#define CO_RETURN_ON_ERROR_WRAP(originResult, code, fmt_str, ...)                                                 \
  do {                                                                                                            \
    auto &&_sub_result = (originResult);                                                                          \
    if (UNLIKELY(_sub_result.hasError())) {                                                                       \
      co_return MAKE_ERROR_F(code, fmt_str ". origin error: {}" __VA_OPT__(, ) __VA_ARGS__, _sub_result.error()); \
    }                                                                                                             \
  } while (false)

#define CO_RETURN_ON_ERROR_MSG_WRAP(originResult, fmt_str, ...)                                     \
  do {                                                                                              \
    auto &&_sub_result = (originResult);                                                            \
    if (UNLIKELY(_sub_result.hasError())) {                                                         \
      auto _code = _sub_result.error().code();                                                      \
      auto _msg = _sub_result.error().message();                                                    \
      co_return MAKE_ERROR_F(_code, fmt_str ". origin error: {}" __VA_OPT__(, ) __VA_ARGS__, _msg); \
    }                                                                                               \
  } while (false)

#define RETURN_ON_ERROR_WRAP(originResult, code, fmt_str, ...)                                                 \
  do {                                                                                                         \
    auto &&_sub_result = (originResult);                                                                       \
    if (UNLIKELY(_sub_result.hasError())) {                                                                    \
      return MAKE_ERROR_F(code, fmt_str ". origin error: {}" __VA_OPT__(, ) __VA_ARGS__, _sub_result.error()); \
    }                                                                                                          \
  } while (false)

#define RETURN_ON_ERROR_MSG_WRAP(originResult, fmt_str, ...)                                     \
  do {                                                                                           \
    auto &&_sub_result = (originResult);                                                         \
    if (UNLIKELY(_sub_result.hasError())) {                                                      \
      auto _code = _sub_result.error().code();                                                   \
      auto _msg = _sub_result.error().message();                                                 \
      return MAKE_ERROR_F(_code, fmt_str ". origin error: {}" __VA_OPT__(, ) __VA_ARGS__, _msg); \
    }                                                                                            \
  } while (false)

#define RUNTIME_ASSERT_RESULT(result, fmt_str, ...)                                                         \
  do {                                                                                                      \
    auto &&_result = (result);                                                                              \
    XLOGF_IF(FATAL, _result.hasError(), fmt_str ". error: {}" __VA_OPT__(, ) __VA_ARGS__, _result.error()); \
  } while (false)

#define RETURN_ON_ERROR(result)         \
  do {                                  \
    auto &&_result = (result);          \
    if (UNLIKELY(_result.hasError())) { \
      RETURN_ERROR(_result);            \
    }                                   \
  } while (0)

#define RETURN_AND_LOG_ON_ERROR(result)         \
  do {                                          \
    auto &&_result = (result);                  \
    if (UNLIKELY(_result.hasError())) {         \
      XLOGF(ERR, "error: {}", _result.error()); \
      RETURN_ERROR(_result);                    \
    }                                           \
  } while (0)

#define CHECK_RESULT(name, expr)           \
  auto &&name##Result = (expr);            \
  if (UNLIKELY(name##Result.hasError())) { \
    RETURN_ERROR(name##Result);            \
  }                                        \
  auto &name = *name##Result

#define CO_RETURN_ERROR(result)                                                                      \
  do {                                                                                               \
    assert(result.hasError());                                                                       \
    if (result.error().message().empty())                                                            \
      co_return hf3fs::makeError(result.error().code(),                                              \
                                 fmt::format("{}:{}, '{}' has error", __FILE__, __LINE__, #result)); \
    else                                                                                             \
      co_return hf3fs::makeError(std::move(result.error()));                                         \
  } while (0)

#define CO_RETURN_ON_ERROR(result)      \
  do {                                  \
    auto &&_result = (result);          \
    if (UNLIKELY(_result.hasError())) { \
      CO_RETURN_ERROR(_result);         \
    }                                   \
  } while (0)

#define CO_RETURN_AND_LOG_ERROR(result)      \
  do {                                       \
    XLOGF(ERR, "error: {}", result.error()); \
    co_return result;                        \
  } while (0)

#define CO_RETURN_AND_LOG_ON_ERROR(result)      \
  do {                                          \
    auto &&_result = (result);                  \
    if (UNLIKELY(_result.hasError())) {         \
      XLOGF(ERR, "error: {}", _result.error()); \
      CO_RETURN_ERROR(_result);                 \
    }                                           \
  } while (0)

#define INLINE_TYPED_RUN(TYPE, code_block) [&]() -> TYPE code_block()
#define CO_INLINE_TYPED_RUN(TYPE, code_block) [&]() -> CoTask<TYPE> code_block()
#define INLINE_RUN(code_block) [&] code_block()

namespace hf3fs {
template <typename T>
using Result = folly::Expected<T, Status>;

template <typename T>
struct IsResult : std::false_type {};

template <typename T>
struct IsResult<Result<T>> : std::true_type {};

using Void = folly::Unit;

template <typename... Args>
[[nodiscard]] inline folly::Unexpected<Status> makeError(Args &&...args) {
  return folly::makeUnexpected(Status(std::forward<Args>(args)...));
}

template <typename T>
[[nodiscard]] inline status_code_t getStatusCode(const Result<T> &result) {
  return result.hasError() ? result.error().code() : StatusCode::kOK;
}

}  // namespace hf3fs

FMT_BEGIN_NAMESPACE

template <>
struct formatter<folly::Unit> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const folly::Unit &, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "Void{{}}");
  }
};

template <typename T>
struct formatter<hf3fs::Result<T>> : formatter<T> {
  template <typename FormatContext>
  auto format(const hf3fs::Result<T> &result, FormatContext &ctx) const {
    if (result.hasError()) return fmt::format_to(ctx.out(), "{}", result.error());
    return formatter<T>::format(result.value(), ctx);
  }
};

template <>
struct formatter<folly::Unexpected<hf3fs::Status>> : formatter<hf3fs::Status> {
  template <typename FormatContext>
  auto format(const folly::Unexpected<hf3fs::Status> &status, FormatContext &ctx) const {
    return formatter<hf3fs::Status>::format(status.error(), ctx);
  }
};

FMT_END_NAMESPACE
