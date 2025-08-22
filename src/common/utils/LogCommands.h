#pragma once

#include <fmt/format.h>
#include <folly/logging/xlog.h>

#include "Coroutine.h"

namespace hf3fs::_detail {
template <typename T>
struct CastResult {
  using type = Result<T>;
};

template <typename T>
struct CastResult<Result<T>> {
  using type = Result<T>;
};

template <>
struct CastResult<void> {
  using type = Result<Void>;
};

template <typename T>
struct CastResult<CoTask<T>> {
  using type = CoTask<typename CastResult<T>::type>;
};

template <typename F>
requires(!IsCoTaskV<F>) typename CastResult<std::invoke_result_t<F>>::type invoke(F &&f) {
  using RT = std::decay_t<decltype(f())>;
  if constexpr (std::is_void_v<RT>) {
    f();
    return Result<Void>(Void{});
  } else {
    return f();
  }
}

template <typename F>
requires(IsCoTaskV<F>) typename CastResult<std::invoke_result_t<F>>::type invoke(F &&f) {
  using RT = std::invoke_result_t<F>;
  if constexpr (std::is_same_v<RT, CoTask<void>>) {
    co_await f();
    co_return Void{};
  } else {
    co_return co_await f();
  }
}
}  // namespace hf3fs::_detail

#define LOG_COMMAND(NORMAL_LEVEL, desc, command)                   \
  do {                                                             \
    auto &&_desc = (desc);                                         \
    XLOGF(NORMAL_LEVEL, "{} ...", _desc);                          \
    auto _res = hf3fs::_detail::invoke([&] { return command; });   \
    using RT = std::decay_t<decltype(_res)>;                       \
    static_assert(std::is_same_v<RT, hf3fs::Result<hf3fs::Void>>); \
    if (UNLIKELY(_res.hasError())) {                               \
      XLOGF(ERR, "{} failed: {}", _desc, _res.error());            \
    } else {                                                       \
      XLOGF(NORMAL_LEVEL, "{} finished", _desc);                   \
    }                                                              \
  } while (false)

#define RETURN_ON_ERROR_LOG_WRAPPED(NORMAL_LEVEL, desc, command)   \
  do {                                                             \
    auto &&_desc = (desc);                                         \
    XLOGF(NORMAL_LEVEL, "{} ...", _desc);                          \
    auto _res = hf3fs::_detail::invoke([&] { return command; });   \
    using RT = std::decay_t<decltype(_res)>;                       \
    static_assert(std::is_same_v<RT, hf3fs::Result<hf3fs::Void>>); \
    if (UNLIKELY(_res.hasError())) {                               \
      XLOGF(ERR, "{} failed: {}", _desc, _res.error());            \
      RETURN_ERROR(_res);                                          \
    } else {                                                       \
      XLOGF(NORMAL_LEVEL, "{} finished", _desc);                   \
    }                                                              \
  } while (false)

#define CO_RETURN_ON_ERROR_LOG_WRAPPED(NORMAL_LEVEL, desc, command)                   \
  do {                                                                                \
    auto &&_desc = (desc);                                                            \
    XLOGF(NORMAL_LEVEL, "{} ...", _desc);                                             \
    auto _res = co_await hf3fs::_detail::invoke([&] { co_return co_await command; }); \
    using RT = std::decay_t<decltype(_res)>;                                          \
    static_assert(std::is_same_v<RT, hf3fs::Result<hf3fs::Void>>);                    \
    if (UNLIKELY(_res.hasError())) {                                                  \
      XLOGF(ERR, "{} failed: {}", _desc, _res.error());                               \
      CO_RETURN_ERROR(_res);                                                          \
    } else {                                                                          \
      XLOGF(NORMAL_LEVEL, "{} finished", _desc);                                      \
    }                                                                                 \
  } while (false)

#define LOG_RESULT(NORMAL_LEVEL, command, ...)                                \
  do {                                                                        \
    auto &&_result = (command);                                               \
    if (_result.hasError())                                                   \
      XLOGF(ERR, "{} failed: {}", fmt::format(__VA_ARGS__), _result.error()); \
    else                                                                      \
      XLOGF(NORMAL_LEVEL, "{} finished", fmt::format(__VA_ARGS__));           \
  } while (false)

#define CO_RETURN_AND_LOG_RESULT(NORMAL_LEVEL, command, ...) \
  do {                                                       \
    auto &&_result2 = (command);                             \
    LOG_RESULT(NORMAL_LEVEL, _result2, __VA_ARGS__);         \
    co_return _result2;                                      \
  } while (false)

#define RETURN_AND_LOG_RESULT(NORMAL_LEVEL, command, ...) \
  do {                                                    \
    auto &&_result2 = (command);                          \
    LOG_RESULT(NORMAL_LEVEL, _result2, __VA_ARGS__);      \
    return _result2;                                      \
  } while (false)
