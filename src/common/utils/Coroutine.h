#pragma once

#include <common/utils/Result.h>
#include <folly/Unit.h>
#include <folly/experimental/coro/Task.h>
#include <folly/experimental/coro/WithCancellation.h>
#include <type_traits>

namespace hf3fs {
template <typename T>
using CoTask = folly::coro::Task<T>;

template <typename T>
using CoTryTask = CoTask<std::conditional_t<std::is_void_v<T>, hf3fs::Result<Void>, hf3fs::Result<T>>>;

template <typename T>
struct IsCoTask : std::false_type {};

template <typename T>
struct IsCoTask<CoTask<T>> : std::true_type {};

template <typename T>
inline constexpr bool IsCoTaskV = IsCoTask<T>::value;

template <typename T>
struct RemoveCoroutine {
  using type = T;
};

template <typename T>
struct RemoveCoroutine<CoTask<T>> {
  using type = T;
};

using CancellationToken = folly::CancellationToken;
using CancellationSource = folly::CancellationSource;
using OperationCancelled = folly::OperationCancelled;

}  // namespace hf3fs
